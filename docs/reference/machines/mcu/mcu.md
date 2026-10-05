# The MCU family — Macintosh Quadra 700/900/950

**Contents:**

1. [Overview & membership](#1-overview--membership) — the generation, its two boot ROMs, the custom-IC
   set, what this page covers against the machine and device pages
2. [Board architecture common to the family](#2-board-architecture-common-to-the-family) — the three
   buses, JDB/Relayer, the MCU/Orwell memory controller and its register file, YANCC, the ROM
   overlay, the low-speed I/O split
3. [Memory map & address decode shared by the family](#3-memory-map--address-decode-shared-by-the-family)
   — the 32-bit physical map, the I/O island, RAM banks and stitching, slot 9, slot E and the PDS,
   the error contract
4. [Device roster](#4-device-roster) — every chip on the board and where its page lives
5. [Interrupt, bus, and clock architecture](#5-interrupt-bus-and-clock-architecture) — the interrupt
   levels and the A/UX remap, VIA2's slot bank, bus masters and arbitration, cache coherency,
   clocks, reset and power
6. [Per-machine index](#6-per-machine-index) — the Quadra 700, 900, 950, and the Workgroup Server 95
7. [Open questions](#7-open-questions)

---

## 1. Overview & membership

### 1.1 What the family is

The **MCU family** is Apple's first 68040 generation: the three original Macintosh Quadras — the
**Quadra 700**, the **Quadra 900** (both October 1991) and the **Quadra 950** (March 1992) — three
main logic boards built around one custom-chip set. Apple's own framing of the generation is the
Quadra 700's feature list: *a Motorola MC68040 microprocessor running at 25 MHz*, 1 MB of ROM
soldered down with a 1 MB ROM SIMM socket for expansion, *two banks of dynamic RAM*, built-in video
with a *direct-access frame buffer and separate video RAM*, an improved SCSI interface, built-in
Ethernet via AAUI, two NuBus slots *with NuBus '90 features*, a processor-direct slot, and sound
I/O like the Macintosh IIsi [1] §"Summary of major features" pp. 1–2. The Quadra 900 adds the
tower packaging, five NuBus slots, four DRAM banks, dual SCSI controllers and the IIfx-style
intelligent I/O processors [2] §"Summary of hardware features" pp. 2–3; the Quadra 950 is, in
Apple's words, *"basically a higher-performance Macintosh Quadra 900"* — the same tower board at
33.333 MHz with faster VRAM and a 16-bit video mode [3] §"Summary of features" p. 5, §Chapter 1 p. 1.

The family is named after its **MCU** — Apple's *Memory Control Unit*, "a custom IC that connects
to the system bus and controls regular and burst-mode data transfers to and from the main RAM and
ROM" [1] §"Custom ICs" p. 8 — because that one part defines what the generation is: the first
Macintosh memory controller built for the 68040's burst transfers and the first with the
access-triggered ROM-at-zero overlay (§2.9). On the Quadra 700 schematic the part carries its
engineering name, **Orwell Memory Controller** [7]; Apple's published documents call it only the
MCU, and this page uses **MCU/Orwell** for the part. Around it sit the **JDB** and **Relayer** I/O
adapter pair, the **YANCC** NuBus controller, and the **DAFB** frame-buffer controller — the four
new custom ICs every board in the family carries [1] pp. 7–8, [2] p. 9.

Everything in this generation hangs off three buses, a structure both developer notes state the
same way: *"There are three main buses in the Macintosh Quadra 700 computer: the system bus, the
I/O bus, and NuBus"* [1] §"Design architecture" p. 4; *"There are three buses in the Macintosh
Quadra 900 computer"* [2] §"Design architecture" p. 7. The system bus connects directly to the
pins of the MC68040 and runs at the processor's clock rate (25 MHz; 33.333 MHz on the 950); the
I/O bus is the asynchronous 15.6672 MHz Macintosh IIfx-style bus (24.28416 MHz on the 950); NuBus
runs at its standard 10 MHz with the NuBus '90 double-rate clock added (§5.5).

This page is the **family doc** of the three-level machine set: it holds what every board shares.
The built-in video controller has its own device pages — [dafb.md](dafb.md) for the part and
[dafb-modes.md](dafb-modes.md) for the display-mode tables — and is cited here by section, never
restated. The three machines have their own pages (§6). The MCU/Orwell itself has **no device
page of its own: its full register treatment lives here** (§2.3–§2.7), because the memory
controller is the family rather than a part one board borrows. The same is true, at the depth the
evidence allows, of JDB/Relayer (§2.2) and YANCC (§2.8).

### 1.2 Membership and the two ROMs

| Machine | Board/project name | CPU, clock | Gestalt type | Boot ROM | NuBus slots | RAM banks | SCSI | Low-speed I/O | Page |
|---|---|---|---:|---|---:|---:|---|---|---|
| Macintosh Quadra 700 (Oct 1991) | Spike [5], [7] | MC68040, 25 MHz | 22 | $420DBFF3 [5] | 2 (D, E) | 2 (one soldered) | one 53C96 | direct | [q700.md](q700.md) / §6.1 |
| Macintosh Quadra 900 (Oct 1991) | Eclipse [8] | MC68040, 25 MHz | 20 | $420DBFF3 [5] | 5 (A–E) | 4 (SIMM) | two 53C96 | IOPs + Caboose | [q900.md](q900.md) / §6.2 |
| Macintosh Quadra 950 (Mar 1992) | Eclipse board, revised [3] p. 6 | MC68040, 33.333 MHz | 26 | $3DC27823 [6] | 5 (A–E) | 4 (SIMM) | two 53C96 | IOPs + Caboose | [q950.md](q950.md) / §6.3 |
| AppleWorkgroup Server 95 (1993) | Quadra 950 board + PDS card | MC68040, 33.333 MHz | as shipped server | Q950 family [6] | 5 (A–E) | 4 | two + card channels | IOPs + Caboose | §6.4 |

Two facts about the ROMs matter at family level. First, **the Quadra 700 and the Quadra 900 run
the same 1 MB boot ROM image** (version `$420DBFF3`, October 1991), and the ROM distinguishes the
machines at run time — the same image must drive both the direct low-speed I/O of the 700 and the
IOP/Caboose organization of the 900 (*observed*: the ROM's identify code reads model-sense bits on
VIA1 port A before selecting machine-specific paths; the two boards' programs are visible as
different strap values, §2.10). The 950 carries its own ROM, `$3DC27823` (March 1992), adding its
video and identity differences [6]. Second, the ROM software is structurally one program for the
family: *"The Macintosh Quadra 700 ROM is a 1 MB ROM device"* whose *"first half … is an overpatch
of the ROM used in the Macintosh IIci"* with the second half *"new code and resources needed to
support the Macintosh Quadra 700"* [1] §"ROM memory map" p. 48 — and the SCSI support is built
deliberately machine-agnostic: *"Because the same ROM code will be used in future models that do
not use that IC, the ROM software that supports the SCSI Manager is a separate module addressed
by a vector that is set up at startup time"* [1] §"Support for SCSI controller" p. 45.

### 1.3 The custom-IC set

The parts every board carries, with the machines' shared and distinct inventory. Apple's lists:
[1] §"Custom ICs" pp. 7–8, [2] §"Custom ICs" p. 9.

| Part | Kind | Role | Page |
|---|---|---|---|
| MCU / Orwell | Apple custom | memory controller: DRAM/ROM control, bank placement, burst support, reset overlay | §2.3–§2.7 |
| JDB (Junction Data Bus) | Apple custom | I/O adapter data half: dynamic bus sizing, byte-lane routing, reset distribution | §2.2 |
| Relayer | Apple custom | I/O adapter control half: chip selects, DSACK, timing conversion, arbitration, watchdog, VIA clock | §2.2 |
| YANCC (Yet Another NuBus Controller Chip) | Apple custom | system-bus/NuBus bridge, write buffer, block transfers, error interrupt | §2.8 |
| DAFB (Direct Access Frame Buffer) | Apple custom | built-in video: VRAM controller, CRTC, RAMDAC interface, TurboSCSI timing assist | [dafb.md](dafb.md) |
| Enhanced ASC (EASC) | Apple custom | sound generator, ASC-compatible | [asc.md](../../hardware/asc.md) |
| Sporty | Apple custom | sound output amplification, replaces the two Sony sound ICs | §4.3 |
| DFAC (Digitally Filtered Audio Chip) | Apple custom (LC-era) | sound input: antialiasing filter, ADC | §4.3 |
| Caboose | Apple custom (towers) | keyswitch, system power, RTC, PRAM, sound control | §6.2 |
| IOP (I/O Processors) | Apple 343S1021 (IIfx part) | serial, floppy and ADB off-load (towers) | [iop.md](../iifx/iop.md) |
| NCR 53C96 | third party | SCSI controller(s) | [ncr-53c96.md](../../hardware/scsi/ncr-53c96.md) |
| DP83932 SONIC | National Semiconductor | Ethernet controller, an I/O-bus bus master | [sonic.md](../../hardware/sonic.md) |
| AC842 / AC842a | Apple custom | palette/DAC (RAMDAC) behind DAFB | [dafb.md](dafb.md) §2.4 |
| DP8531 | National Semiconductor | pixel-clock synthesizer behind DAFB | [dafb.md](dafb.md) §2.5 |
| VIA1, VIA2 | 6522-class | interrupt concentrators, timers, system control | [via.md](../../hardware/via.md) |
| SWIM | Apple custom (shared) | floppy control | [swim.md](../../hardware/swim.md) |
| SCC | Zilog Z8530-class | serial (direct on the 700) | [scc.md](../../hardware/scc.md) |
| RTC/PRAM | classic part | real-time clock and parameter RAM | [rtc.md](../../hardware/rtc.md) |

## 2. Board architecture common to the family

### 2.1 The three buses

**The system bus** *"connects directly to the pins of the MC68040 microprocessor and runs at the
processor's clock rate, 25 MHz"* on the 700 and 900 [1] p. 4; on the 950 it runs at 33.333 MHz, with
the processor *"receiv[ing] both the bus clock and a 2X bus clock for internal timing"* — 66.666
MHz internal [3] §"Faster processor clock" p. 5. Four controller ICs sit on it: the MCU, YANCC,
DAFB and the SCSI controller(s) [1] p. 4, [2] p. 7 (the towers' second 53C96 makes five). The
PDS connector is wired to these same pins with no buffers (§3.5). Alternate masters exist — NuBus
masters through YANCC, the SONIC, PDS cards — and cache snooping is not used anywhere in the
family (§5.4).

**The I/O bus** is inherited from the Macintosh IIfx: *"The I/O bus enables the Macintosh Quadra
700 computer to use the same I/O device controllers used in previous Macintosh computers. The I/O
bus clock runs at 15.6672 MHz and is completely asynchronous to the system bus clock"* [1]
§"I/O bus adapter ICs: JDB and Relayer" p. 10, [2] p. 10. On the 950 it runs at 24.28416 MHz —
Apple describes it loosely as a 25 MHz I/O bus — *"to accommodate the faster I/O bus clock, the
Macintosh Quadra 950 uses a 25 MHz version of the Sonic"* [3] §"Faster I/O bus" p. 7. The I/O bus
carries the VIAs, the SONIC register window, the Enhanced ASC, and either the direct SCC/SWIM/RTC
(700) or the IOP host apertures (towers); its peripheral-access conventions are the II-family
ones (§3.2).

**NuBus** runs at the standard 10 MHz with selected NuBus '90 features added: a 20 MHz clock for
double-rate block transfers *between cards* (double-rate transfers *"to or from the main memory"*
are explicitly not supported [1] §"NuBus slots" p. 23), the /TM2 transfer-mode line, and terminated
-but-undriven serial-bus and cache-coherency signal pins (§5.5, and
[sonic.md](../../hardware/sonic.md) §1.3 for the bus context).

### 2.2 JDB and Relayer — the I/O adapter

Between the system bus and the I/O bus sits a two-chip **I/O adapter**, split *"because of the high
pin count required"* [1] p. 10, [2] p. 10. Apple gives the two chips' function lists verbatim,
and the lists are the whole published specification of the pair:

| IC | Functions [1] p. 10, [2] p. 10 |
|---|---|
| JDB (Junction Data Bus) | controlling the data path (dynamic bus sizing and data byte lane routing); synchronizing and distributing the reset signals |
| Relayer | generating chip select and DSACK signals for devices on the I/O bus; converting timing signals between the system bus and the I/O bus; arbitrating between the system bus and the I/O bus; acting as watchdog for bus activity and timeout; controlling the address-bus transceiver ICs; generating the clock signal for the VIA ICs |

*"The ICs making up the I/O bus adapter contain no programmable registers and do not require
support from the system software"* [1] p. 10, [2] p. 10 — the pair is invisible to software except
as access timing and as the arbiter of the I/O bus (§5.3). Relayer's watchdog is one of the
family's two error paths (§3.6); its VIA clock output is what times the VIA timers (§5.5). The
Quadra 700 PDS connector exposes the adapter's reset distribution to expansion cards as
**/MEMRESET**, a *"fast reset generated by JDB IC for Memory Control Unit IC"*, an output of the
main board [1] Table 2-3 p. 30.

### 2.3 MCU/Orwell — the memory controller

Apple's complete published description of the part's role is two sentences plus a bank rule:

> *"The MCU connects to the system bus and provides control and timing signals for ROM and RAM.
> The MCU supports all types of MC68040 memory access, including burst modes."* [1]
> §"Memory Control Unit" p. 10; [2] §"Memory Control Unit" p. 10

and, for RAM: *"The MCU contains registers that the system software uses to set the starting
address of each bank of memory. At startup time, the system software determines the sizes of the
banks and assigns the bank starting addresses so that the banks occupy contiguous memory
spaces"* [1] §"RAM control" p. 11; [2] p. 11. The ROM-side view adds the consequence: *"At startup
time, the ROM software determines the amount of RAM installed in each bank and stores the actual
bank sizes in registers in the Memory Control Unit IC. Using those bank sizes, the Memory Control
Unit IC decodes bus addresses so that the separate physical banks of RAM occupy contiguous
addresses in logical memory space"* [1] §"Support for Memory Control Unit" p. 45. The card-design
book's summary is the sharpest statement of the design intent: when the machine is powered on,
*"the start code in the ROM physically 'stitches' the memory together so that all the bank
addresses are contiguous"*, and *"The Memory Control Unit … supports all 68040 memory-access
types, including burst reads and burst writes"* [4] Part II §"RAM" p. 28.

The part's external obligations, all Apple-documented, are:

- **ROM control** — the 1 MB boot ROM (two 4-Mbit devices, each 256K × 16-bit, 150 ns) with a ROM
  SIMM socket that, when populated, automatically disables the on-board ROM [1] p. 11, [2] p. 11;
- **the reset overlay** — ROM mapped at $00000000 out of reset, restored by the first normal-ROM
  access (§2.9);
- **bank decode** — up to four banks, each occupying a 64 MB physical decode window, with
  programmable bank starting addresses (§2.5, §3.3);
- **burst support** — every 68040 access type including burst reads and writes [1] p. 10, [4] p. 28;
- **the PDS handshake** — the PDS card's /MI.SLOT (*"memory inhibit from PDS card to Memory
  Control Unit IC"*) and the JDB's /MEMRESET both land in the MCU [1] Table 2-3 p. 30.

Schematics add the engineering name — the Quadra 700 board labels the device **Orwell Memory
Controller** [7] — and parity-related test signal wiring on the DRAM interface that the developer
notes never surface as a software-visible parity subsystem (§7).

### 2.4 Orwell: the register window at $5000E000

Apple publishes the MCU's control block position but not one register inside it. The card-design
book's Quadra address-map table carries the line *"Memory Control Unit controls"* against
**$5000E000–$5000EFFF** [4] Table 16-5 p. 375; the developer notes' own I/O map figures show the
same block [1] Figure 1-2 p. 9, [2] Figure 1-2 p. 12. Nothing else is documented: no register
table, no bit definitions, no reset values appear in any Apple source in evidence. The
reconstruction below is therefore built from two directions — the boot ROM's own programming
(*observed*, in the boot-time access trace of the $420DBFF3 image against a logging register
window [5]) and a bit-level reading of the ROM image's memory-sizing code — and every assignment
that is not directly witnessed is marked. The window is byte-offsetted within $5000E000–$5000EFFF;
the offsets below are byte offsets.

| Offset range | Function | Provenance |
|---|---|---|
| +$000–+$087 | the serial configuration register, one bit per longword (§2.5) | recovered from the ROM's programming; *inferred — unverified* |
| +$0A0–+$0BF | latch strobes: commit staged speed/bank/refresh selections (§2.6) | *observed*: written after every programming burst [5] |
| +$100 and above | parity/status latches | *inferred — unverified*; never touched by the boot ROM [5] |

### 2.5 Orwell: the serial configuration register

The MCU has a single configuration data pin, and its register file is arranged around that fact:
**configuration bit N is addressed as the longword at byte offset N × 4, and only bit 0 of the
accessed longword carries data**. A staged configuration is written one bit at a time — the ROM
writes a longword per bit and the data rides the least significant byte lane (offset N × 4 + 3) —
and *a staged value takes effect only when a matching latch address is poked* (§2.6). The
recovered bit assignment, thirty-four bits wide:

| Bits | Field (recovered) | Notes |
|---|---|---|
| 0–5 | bank B start address | count in 4 MB units; power-up value $10 → 64 MB |
| 6–11 | bank C start address | power-up value $20 → 128 MB |
| 12–17 | bank D start address | power-up value $30 → 192 MB |
| 18 | system clock speed select | 25/33.333 MHz class |
| 19–20 | DRAM access-time class | the 80 ns SIMM requirement made programmable |
| 21–23 | ROM access speed | matches the 150 ns ROM devices |
| 24–26 | DRAM refresh interval | |
| 27 | parity enable | parity signals exist on the schematic [7]; no software uses them (§7) |
| 28 | parity select (odd) | |
| 29–33 | page-mode / wait-state / RAS-precharge timing | |

Bank A is not programmable: it is always at physical address 0, which is what makes the Quadra
700's 4 MB of soldered RAM a fixed first bank. A bank field counts 4 MB units, so the 64 MB
physical window splits of §3.3 are the values $10/$20/$30. Every field assignment above is
*inferred — unverified* against silicon: it is consistent with the observed programming (§2.7) and
with the bank geometry Apple documents, but no Apple document, ROM listing or hardware capture in
evidence states a single one of these bits outright.

### 2.6 Orwell: latch strobes, speed and refresh fields, status

The latch area at +$0A0 is a set of write-only strobes: an access there commits the staged
configuration bits it owns. The boot trace shows the ROM writing +$0A0-class addresses with
full-width data patterns ($0000FFFF, $FFFFFFFF, $FF00FFFF) immediately after each configuration
burst — the data value is irrelevant to a strobe, and the important fact is the address and its
ordering (*observed*, [5]). The recovered grouping is: banks at +$0A0, speed selections at +$0A4,
refresh at +$0A8, and further strobes through +$0BC (*inferred — unverified*). Reads of the latch
area do not return data. At +$100 and above sit parity-status and error latches the boot ROM never
touches [5] (*observed* by absence) — the DRAM parity wiring visible on the schematic [7] is
presumably what they report, but no software in evidence reads them (§7).

### 2.7 Orwell: the boot-time programming sequence (observed)

The memory-sizing bring-up, recorded instruction-level against the $420DBFF3 image on a Quadra
700 configuration (4 MB soldered + 4 MB SIMMs) [5]:

1. **Early reads, twice.** The start-up code reads the window's first longword (+$000–+$003) while
   executing from the ROM overlay at $0000315E — then, after the overlay-dropping access, reads
   the same offsets again from the ROM's normal aperture at $4080315E: the same early sequence
   runs first from the overlaid image and again from the ROM repeat at $40800000 (*observed*).
2. **The power-up split.** One burst writes thirty-two longwords at +$000 through +$07C, then one
   at +$084. The written values form a halving chain — $138B0810, $09C58408, $04E2C204, … down to
   $00000000, each longword the previous one shifted right by one — and because the data bit is
   each longword's least significant byte lane, the chain serializes exactly the configuration
   bits 4, 11, 16, 17, 19, 23, 24, 25 and 33 (*observed*, [5]). Under the §2.5 assignment those
   decode as bank B/C/D starts of $10/$20/$30 — the 64 MB-boundary split — plus the 80 ns DRAM
   class, a ROM-speed field of 4, a refresh field of 3, and one page-mode/wait-state bit: the
   machine's timing configuration stated one bit per longword.
3. **The latch strobes.** Longword writes to +$0A0–$0BC follow the burst (*observed*).
4. **Reprogramming during sizing.** Later bursts re-write the same bank-split values, then
   progressively smaller chains (the values walk down as the sizing code walks the bank geometry),
   each followed by its +$0A0 strobe — the *"stitches"* of [4] p. 28 in action, ending with the
   discovered bank starts programmed contiguously (*observed*, [5]; absolute values are
   configuration-dependent — the trace is the 8 MB machine).

The sequence is the family's RAM bring-up contract: a re-implementation that ignores the window
entirely and presents flat RAM will boot supported configurations, but it cannot reproduce the
sizing probes, the 64 MB-window behaviour of partially filled banks, or any of the timing fields.

### 2.8 YANCC — the NuBus controller

YANCC (*"Yet Another NuBus Controller Chip"*) is the system-bus/NuBus bridge: *"three chips
comprise the interface between the system bus and the NuBus: the YANCC IC and two 16-bit
transceiver ICs"*, the transceivers the same type as the Macintosh IIci's [1]
§"NuBus controller IC: YANCC" p. 12, [2] p. 13. Its documented feature set:

- support for all types of single data transfers in either direction;
- *"a buffer, one long word deep, for pending writes from the MC68040 to the NuBus"*;
- support for block move transfers between NuBus masters and main memory;
- support for pseudoblock transfers between the MC68040 and NuBus slaves;
- support for some functions defined in the NuBus '90 specification [1] p. 12, [2] p. 13.

Unlike earlier Macintosh NuBus controllers, *"the YANCC IC generates an interrupt when there is an
error involving the write buffer. Software controls this interrupt by means of a control and
status register in the YANCC"* [1] p. 12, [2] p. 13 — and the interrupt mapping table places that
error at **CPU level 7, shared with the NMI** (*"NMI/YANCC err"*, [1] Table 2-7 p. 38, [2]
Table 2-7 p. 41; §5.1). The control/status register's address is published — *"YANCC controls"*
at **$50028000–$50029FFF** [4] Table 16-5 p. 375 — but its width and bit layout are not (§7).
YANCC is also the recipient of the PDS slot claim: a pseudoslot PDS card notifies *"the NuBus
controller that it is using the NuBus space"* by asserting /PDS.SLOT.E.EN, so YANCC ignores
accesses to slot $E [1] Table 2-3 p. 30, [4] §"Pseudoslot design guidelines" p. 376. The
Quadra-family slots support the NuBus '90 *slave* block-transfer enhancements earlier Macintoshes
lacked [4] Part I §"NuBus expansion" p. 3; the double-rate (2X clock) form is card-to-card only
[1] p. 23, [2] §"NuBus '90 features" p. 26.

### 2.9 The boot ROM and the overlay

Every machine in the family boots from 1 MB of ROM: *"The Macintosh Quadra 700 computer uses a
1 MB ROM. The first half of the ROM is an overpatch of the 512 KB ROM used in other members of
the Macintosh-II family. The second half of the ROM is new software to support the new features"*
[1] §Chapter 3 p. 48, [2] §Chapter 3 p. 42; the 950 carries its own image [6]. The overlay that
starts the machine is the MCU's, and Apple describes it exactly:

> *"When the computer is reset, the MCU maps ROM addresses into memory beginning at address
> $0000 0000 and disables the system RAM. As soon as the ROM code addresses the normal ROM space
> ($4000 0000), the MCU automatically remaps the ROM to its normal addresses and restores RAM
> addressing starting at $0000 0000."* [1] §"ROM control" p. 11; [2] p. 11

This is an **access-triggered overlay** — no VIA bit and no software write removes it, unlike the
GLUE/MDU-era machines' vOverlay line; the first access to the $40000000–$4FFFFFFF aperture is
itself the trigger, and the triggering access completes as a ROM access (*observed*: in the boot
trace, the same early sequence executes first from the overlay and then from the normal aperture
[5], §2.7). The 1 MB image repeats throughout the 256 MB ROM aperture [4] Table 16-5 p. 375; what
aliases above the first megabyte while the overlay is armed is not established (§7).

### 2.10 The low-speed I/O split: direct (Quadra 700) vs IOP (towers)

The one architectural split inside the family is the low-speed I/O organization, and it follows
the board pairing exactly. The Quadra 700 wires its SCC, SWIM and RTC **directly** — its
comparison-table rows read "SCC", "SWIM", "ADB: same as Macintosh IIci" [1] Table 1-4 p. 26 —
while the Quadra 900/950 put the same functions behind the Macintosh IIfx's **IOPs**: *"Like the
Macintosh IIfx, the Macintosh Quadra 900 has intelligent I/O processors (IOPs) on the ports for
the floppy disk, Apple Desktop Bus (ADB), and serial I/O to relieve the main processor of
routine I/O tasks"* [2] §"Introduction" p. 2, [3] §"Summary of features" p. 5. The towers' rows
read "SCC/IOP", "SWIM/IOP", "ADB: same as Macintosh IIfx" [1] Table 1-4 p. 26, [2] Table 1-4
p. 29. The IOPs are the IIfx parts — Apple PIC/IOP host apertures with the IIfx register layout
(see [iop.md](../iifx/iop.md), "Host address map") — and their host windows replace the 700's
direct SCC and SWIM blocks in the I/O island (§3.2).

The towers also carry **Caboose**, *"a custom processor that manages the keyswitch, system power,
the real-time clock, and parameter RAM"* [2] §"Custom ICs" p. 9 — the card-design book adds the
sound control duty and notes it is *"used only in the Macintosh Quadra 900"* [4] Part II
§"Macintosh Quadra-family computers" p. 20. The surviving Quadra 900 production schematic carries
`EGRET_*` signal names on those nets [8], so the part is Egret-protocol-compatible in its host
interface; which exact microcontroller and firmware Apple shipped is not established (§7). The
Q700/Q900 identity question — one ROM, two low-speed organizations — is settled by model-sense
straps: the start-up code reads VIA1 port A before selecting machine paths, and the boards strap
different patterns there (*observed* in the identify code of both ROM images [5], [6]; the
per-machine values are in the machine pages).

## 3. Memory map & address decode shared by the family

### 3.1 The 32-bit physical map

The card-design book publishes the family's address allocation as a table — the one place Apple
spells the whole map out — and the developer notes' simplified maps agree [1] Figures 1-2/1-3
pp. 9, 14, [2] Figures 1-2/1-3 pp. 12, 14:

| Physical range | Function | Source |
|---|---|---|
| $00000000–$3FFFFFFF | RAM (banks in 64 MB decode windows; overlay at reset, §2.9) | [4] Table 16-5 p. 375 |
| $40000000–$4FFFFFFF | ROM (1 MB image repeated through the 256 MB aperture) | [4] p. 375 |
| $50000000–$53FFFFFF | motherboard I/O space — the low block of §3.2 repeated in images | [4] p. 375 |
| $54000000–$5FFFFFFF | reserved for Apple | [4] p. 375 |
| $60000000–$EFFFFFFF | NuBus super slot space (slot ID on A31–A28), 256 MB per slot | [4] p. 375 |
| $F0000000–$F0FFFFFF | reserved | [4] p. 375 |
| $F1000000–$FFFFFFFF | NuBus standard slot space (slot ID on A27–A24), 16 MB per slot | [4] p. 375 |

The slot formulas are the conventional NuBus ones: super slot $s at $s0000000, standard slot $s at
$F0000000 | (s << 24). Slot $9 belongs to the built-in video (§3.4); the physical connectors take
$D and $E on the Quadra 700 and $A through $E on the towers (§6); slot $E doubles as the PDS
pseudoslot (§3.5).

### 3.2 The I/O island

The defined low I/O block is $50000000–$5003FFFF (256 KiB), with Apple's diagrams repeating the
block in images through $53FFFFFF and the remainder of $50000000–$5FFFFFFF reserved [4]
Table 16-5 p. 375. The per-device allocation, with the two boards' differences:

| Base | Range | Q700 | Q900/Q950 | Page |
|---|---|---|---|---|
| $50000000 | $2000 | VIA1 | VIA1 | [via.md](../../hardware/via.md) |
| $50002000 | $2000 | VIA2 | VIA2 | [via.md](../../hardware/via.md) |
| $50004000 | $4000 | reserved for Apple | reserved for Apple | |
| $50008000 | $2000 | Ethernet station-address PROM | Ethernet PROM | [sonic.md](../../hardware/sonic.md) §1.3 |
| $5000A000 | $2000 | SONIC registers | SONIC registers | [sonic.md](../../hardware/sonic.md) §2.1 |
| $5000C000 | $2000 | SCC (direct) | SCC IOP host aperture | [scc.md](../../hardware/scc.md), [iop.md](../iifx/iop.md) |
| $5000E000 | $1000 | MCU/Orwell controls | MCU/Orwell controls | §2.4 |
| $5000F000 | $400 | SCSI 0 (internal), 53C96 | SCSI 0 (internal) | [ncr-53c96.md](../../hardware/scsi/ncr-53c96.md) §2.1 |
| $5000F400 | $400 | reserved for Apple | SCSI 1 (external), 53C96 | [ncr-53c96.md](../../hardware/scsi/ncr-53c96.md) §2.1 |
| $5000F800 | $6000 | reserved | reserved | |
| $50014000 | $2000 | Enhanced ASC | Enhanced ASC | [asc.md](../../hardware/asc.md) §3.3 |
| $50016000 | $8000 | reserved for Apple | reserved for Apple | |
| $5001E000 | $2000 | SWIM (direct) | SWIM/ADB IOP host aperture | [swim.md](../../hardware/swim.md), [iop.md](../iifx/iop.md) |
| $50020000 | $8000 | reserved for Apple | reserved for Apple | |
| $50028000 | $2000 | YANCC controls | YANCC controls | §2.8 |
| $5002A000 | $16000 | reserved for Apple | reserved for Apple | |

Addresses and block sizes are Apple's [4] Table 16-5 p. 375; the SCSI pseudo-DMA data apertures
follow each controller's register block ([ncr-53c96.md](../../hardware/scsi/ncr-53c96.md) §3.8).
Because the I/O bus exists to reuse *"the same I/O device controllers used in previous Macintosh
computers"* [1] p. 10, the in-block access conventions are the II-family ones: the byte-wide
peripherals keep the classic register-select-on-high-address-bits and upper-data-byte
conventions of the Macintosh II generation — the SCC, for example, answers on the upper byte of
the word as it always has [9] §"SCC" p. 169 — so the register spacing inside each block is
coarser than a flat byte array (a VIA register is selected by address bits well above A3, the
53C96's registers sit on a 16-byte stride [ncr-53c96.md](../../hardware/scsi/ncr-53c96.md) §2.1,
and the SONIC's 16-bit registers ride one half of the 32-bit access [sonic.md](../../hardware/sonic.md)
§2.1). The exact per-block decode granularity — which low address bits each device ignores, and
how the repeated images through $53FFFFFF alias — is recovered rather than documented
(*inferred — unverified*; §7).

### 3.3 RAM: physical banks and logical stitching

Main RAM is organized as banks that each decode a **64 MB physical window** before any
programming: *"Main RAM in the Macintosh Quadra 700 computer consists of two banks that begin on
64 MB boundaries"* [1] §"Support for Memory Control Unit" p. 45; *"Each bank of main RAM occupies
64 MB of physical address space"* [2] §"RAM control" p. 11. The board geometries differ (§6):
the 700 has 4 MB soldered as bank 1 plus four 30-pin SIMM sockets as bank 2 — fast page mode,
80 ns or faster, 1 MB or 4 MB SIMMs, all four equal, for totals of 4, 8 or 20 MB [1] §"RAM
control" p. 11 — while the towers take four banks of four equal 30-pin SIMMs, 1/4/16 MB parts,
for 4 to 64 MB [2] p. 11, with the 950 note listing 4 or 16 MB per bank [3] p. 5. Out of reset
the populated banks sit at their 64 MB boundaries; the ROM then sizes them and programs
contiguous logical starts into the MCU, which re-decodes so that *"the separate physical banks of
RAM occupy contiguous addresses in logical memory space"* [1] p. 45 (the full observed sequence,
§2.7). The behaviour of an access above the installed size but inside a bank's window —
aliasing from unconnected address lines, or an open-bus read — is not established (§7), and
neither is the later-system support for 256 MB-class SIMM arrangements beyond the towers'
documented 64 MB (§7).

### 3.4 Slot 9: built-in video

*"The control registers in the DAFB IC and the frame-buffer VRAM are mapped into the memory
locations that were assigned to NuBus slot $9 in earlier Macintosh models"* [1] §"Video
frame-buffer controller IC: DAFB" p. 16, [2] p. 14. Built-in video is therefore a pseudo-slot
device: the Slot Manager polls it with the expansion slots, its interrupt is one of the VIA2
port A bits (§5.2), and its apertures — the 2 MB VRAM window at $F9000000 and the 1 KB register
window at $F9800000 — are fully specified in [dafb.md](dafb.md) §2.1, with the per-VRAM depth
ceilings in [dafb-modes.md](dafb-modes.md) and the video lineage in
[video-overview.md](../../hardware/video-overview.md) §3. The family-level facts: the frame buffer
is **dedicated VRAM in separate banks**, not main memory (the 700/900 are *"the first Macintosh
models to combine an MMU with built-in video using frame buffers in separate banks of VRAM"*
[1] §"Support for built-in MMU" p. 40), and the 950 runs the same board's video faster — 80 ns
VRAM devices against the 900's 100 ns, *"reprogrammed to take advantage of the faster VRAM"*
[3] §"Faster video hardware" p. 6.

### 3.5 Slot E and the processor-direct slot

The 68040 **processor-direct slot** is wired directly to the MC68040's pins through the system
bus, *"located in line with NuBus slot $E"* and sharing its back-panel opening — using a PDS
card precludes a NuBus card in slot $E [1] §"Processor-direct slot" p. 25, [2] §"Expansion slots"
p. 24. A PDS card has the same dimensions as a NuBus card and a 140-pin KEL connector; the
signals are the microprocessor's own, unbuffered, with strict load limits (40 pF on address,
data and AUX.CPUCLK lines; 20 pF elsewhere) [1] §"PDS card specifications" pp. 25–31.

Its address decode is the pseudoslot scheme: a PDS card *"can have memory locations in the upper
part of the RAM memory space ($1000 0000 through $3FFF FFFF) or in the space assigned to NuBus
slot $E ($FE00 0000 through $FEFF FFFF or $E000 0000 through $EFFF FFFF)"* [4]
§"Memory and I/O access for expansion cards" p. 374. A slot-$E card must decode **both** the
standard and the super slot space, answering unused locations with /TEA, and must assert
**/PDS.SLOT.E.EN** so YANCC ignores slot $E [1] §"PDS card design considerations" p. 29, [4] p.
376. The one genuinely dangerous contract on the platform follows: *"A PDS card that asserts the
/PDS.SLOT.E.EN signal must issue a /TA (Transfer Acknowledge) or a /TEA in response to all
accesses to the $Exxx xxxx and $FExx xxxx address space. (This action will keep the machine from
hanging, since there is no time-out timer for slot $E when the /PDS.SLOT.E.EN signal is
asserted.)*" [1] p. 29, [2] §"PDS card specifications" p. 39. A silent slot $E hangs the machine
outright. PDS cards are 68040-specific — cards designed for 68020/68030 machines *"will not
work"* [1] §"PDS card design considerations" p. 29 — and the 950's slot is electrically the same
design at 33 MHz timing [3] §"Faster PDS" p. 6.

### 3.6 Undecoded space and the error contract

Two failure levels exist for an address nothing answers:

- **Relayer's watchdog.** The I/O adapter's control chip is *"acting as watchdog for bus activity
  and timeout"* [1] p. 10, [2] p. 10 — an I/O-bus cycle that no device terminates times out to a
  bus error. The timeout value is not documented (§7).
- **The slot probe contract.** The Slot Manager's declaration-ROM search treats a faulting slot
  read as an empty slot ([declaration-rom.md](../../hardware/nubus/declaration-rom.md) §11), so
  empty NuBus slots must fail cleanly rather than return data. PDS slot $E is the deliberate
  exception: with /PDS.SLOT.E.EN asserted there is no timeout at all (§3.5).

What an access to a reserved I/O island offset returns — bus error, floating bus, or a stable
value — is not established (§7). Inside the RAM aperture, an access beyond installed capacity
stays inside a bank's 64 MB window and its behaviour is a bank-geometry question (§3.3).

## 4. Device roster

The complete part inventory, grouped by role. This roster states what each part is and where it
lives; registers and behaviour belong to the device pages.

### 4.1 The system-bus core

| Part | Role | Where | Page |
|---|---|---|---|
| MC68040 | processor: 4 KB I-cache, 4 KB D-cache, one instruction and one data MMU, built-in FPU, CopyBack mode, MOVE16 | system bus, 25/33.333 MHz | §1, [1] §"MC68040 microprocessor" p. 5 |
| MCU/Orwell | memory controller, ROM overlay, bank stitching | system bus | §2.3–§2.7 |
| YANCC (+ two 16-bit transceivers) | NuBus bridge, write buffer, block/pseudoblock transfers, error interrupt | system bus / NuBus | §2.8 |
| DAFB (+ AC842/AC842a, DP8531) | built-in video, TurboSCSI timing | system bus, slot-9 space | [dafb.md](dafb.md) |
| NCR 53C96 | SCSI protocol controller; two on the towers | behind DAFB's timing assist | [ncr-53c96.md](../../hardware/scsi/ncr-53c96.md) |

### 4.2 The I/O side

| Part | Role | Where | Page |
|---|---|---|---|
| JDB + Relayer | system/I/O bus adapter, arbitration, watchdog, reset distribution, VIA clock | between the buses | §2.2 |
| VIA1 | level-1 interrupt source, RTC/PRAM interface (700), ADB state lines, model sense, two new family bits (§5.1) | I/O bus $50000000 | [via.md](../../hardware/via.md) |
| VIA2 | level-2 interrupt concentrator: slots, video, Ethernet, SCSI, sound, timers | I/O bus $50002000 | [via.md](../../hardware/via.md) |
| SCC | dual-channel serial — direct on the 700, behind the SCC IOP on the towers | $5000C000 | [scc.md](../../hardware/scc.md), [iop.md](../iifx/iop.md) |
| SWIM | SuperDrive floppy control — direct on the 700, behind the SWIM/ADB IOP on the towers | $5001E000 | [swim.md](../../hardware/swim.md) |
| RTC/PRAM | real-time clock, parameter RAM — on VIA1 on the 700, inside Caboose on the towers | VIA1 / Caboose | [rtc.md](../../hardware/rtc.md) |
| IOPs (towers) | 65C02-based I/O processors for serial and floppy/ADB | $5000C000, $5001E000 | [iop.md](../iifx/iop.md) |
| Caboose (towers) | keyswitch, power, RTC/PRAM, sound control | VIA1 handshake lines | §6.2 |
| SONIC DP83932 | Ethernet controller, the one I/O-bus bus master | $5000A000 (+ PROM $50008000) | [sonic.md](../../hardware/sonic.md) |
| ADB | desktop bus — IIci-style on the 700, IIfx/IOP-style on the towers | VIA1 / SWIM IOP | [adb.md](../../hardware/adb.md) |

### 4.3 Video and sound

| Part | Role | Where | Page |
|---|---|---|---|
| DAFB | frame-buffer controller, CRTC, monitor sense, TurboSCSI assist | $F9000000/$F9800000 | [dafb.md](dafb.md) |
| VRAM banks | four banks; 512 KB (700) or 1 MB (towers) shipped, 2 MB maximum | on the board | [dafb-modes.md](dafb-modes.md) |
| AC842 / AC842a | palette/DAC; the 950's AC842a adds the 16-bit mode | behind DAFB | [dafb.md](dafb.md) §2.4 |
| Enhanced ASC | sound generator, ASC-compatible | $50014000 | [asc.md](../../hardware/asc.md) |
| DFAC | sound input, antialiasing filter, ADC | analog side | §2.10 |
| Sporty | output amplification and digital attenuation, replacing the Sony sound ICs | analog side | [1] §"Sound ICs" pp. 17–18 |

### 4.4 Devices the family does not carry

Worth stating explicitly, because earlier machines carry them: no RBV-class shared-memory video
(DAFB owns dedicated VRAM); no OSS interrupt controller (the towers reuse VIAs, not the IIfx's
programmable part — the IOPs came along, the OSS did not); no SCSI DMA wrapper chip (the 53C96 is
driven through DAFB's TurboSCSI pseudo-DMA aperture, not a bus-master engine —
[ncr-53c96.md](../../hardware/scsi/ncr-53c96.md) §3.8); and no cache-coherency protocol on NuBus
— the NuBus '90 coherency pins are present on the connectors but *"the cache-coherency protocol
defined in the NuBus '90 specification is not implemented"* [4] Part I §"Cache-coherency signals"
p. 59.

## 5. Interrupt, bus, and clock architecture

### 5.1 Interrupt levels and the A/UX remap

All device interrupts are **autovectored**; the 68040 mask discipline is the standard one
(*"When the MC68040 is executing a level x interrupt, it first sets the interrupt mask to level
x"* … restored by RTE) [1] §"Interrupt handling" pp. 36–37, [2] pp. 40–41. The level map, from
both notes' identical tables [1] Table 2-7 p. 38, [2] Table 2-7 p. 41:

| Level | Macintosh mapping | A/UX mapping |
|---|---|---|
| 1 | VIA1 | software interrupt |
| 2 | VIA2 (SCSI, sound, NuBus slots, Ethernet, video) | VIA2 (SCSI, NuBus slots, video) |
| 3 | — | Ethernet |
| 4 | SCC | SCC |
| 5 | — | Sound |
| 6 | — | VIA1 |
| 7 | NMI / YANCC error | NMI / YANCC error |

The remap is hardware: *"In addition to the standard Macintosh II functions, the VIA1 includes
two new bits. The first is a software interrupt signal, and the second is the A/UX interrupt
enable signal. When the software interrupt bit is set, an interrupt will be passed to the
MC68040. When the A/UX interrupt enable bit is set, the interrupt control PAL will remap the
interrupts"* [1] p. 37, [2] p. 41. So the family carries a discrete **interrupt control PAL**
between the VIAs and the 68040's /IPL lines — the PDS connector exposes them as /IPL0–/IPL2,
*"from PAL; not to be used as wire-OR lines"* [1] Table 2-4 p. 30. The dispatch order inside
level 2 is the slot-polling flow of §5.2; within the VIA2 sources the first-level dispatcher
determines which of *"SCSI, sound chip, real-time clock, or expansion slot"* is requesting and
dispatches accordingly [1] pp. 36–37, [2] pp. 40–41.

### 5.2 VIA2: the slot bank and /SLOTIRQ

The slot, video and Ethernet interrupt lines are OR-gated into one signal: *"The [NuBus]
interrupt signals (slot $E interrupt is shared by the 68040 Direct Slot), the built-in video
interrupt signal, and the Ethernet controller interrupt signal are routed through an OR gate to
generate a signal called /SLOTIRQ. This signal is connected to the CA1 input of VIA2"* [1]
§"Interrupt handling" p. 36, [2] p. 40. VIA2 then raises CPU level 2, and the handler *"polls the
second VIA, bits PA0 through PA6, to determine which slot generated the interrupt"* [1] p. 36,
[2] p. 40. The port A bank:

| VIA2 PA bit | Quadra 700 [1] Table 2-6 p. 37 | Quadra 900/950 [2] Table 2-6 p. 40 |
|---|---|---|
| PA0 | Ethernet IRQ | Ethernet IRQ |
| PA1 | not connected | slot $A IRQ |
| PA2 | not connected | slot $B IRQ |
| PA3 | not connected | slot $C IRQ |
| PA4 | slot $D IRQ | slot $D IRQ |
| PA5 | slot $E IRQ (shared with the PDS) | slot $E IRQ (shared with the PDS) |
| PA6 | video IRQ (built-in video, pseudo-slot 9) | video IRQ |

The lines are level images, and the recommended — in practice mandatory — card design is to latch
the slot /IRQ: *"The recommended design practice is to latch the slot /IRQ signal so that once it
is asserted, the interrupt handler software for the card has the responsibility of clearing the
interrupt. This ensures that the slot /IRQ signal is asserted when polled and that the Slot
Manager is dispatched correctly"* [1] p. 37, [2] p. 40. The remaining VIA2 handshake pins carry
the rest of level 2: SCSI, sound and VIA2 timer requests [1] p. 36, and the towers' SWIM/ADB IOP
host interrupt. Per-machine pin assignments are in the machine pages.

### 5.3 Bus masters and arbitration

The family supports four bus masters — three on the system bus, one on the I/O bus — arbitrated
by Relayer:

| Priority | Bus | Master [4] Table 16-4 p. 374 |
|---|---|---|
| First (highest) | I/O bus | Sonic Ethernet controller |
| Second | system bus | YANCC NuBus controller |
| Third | system bus | PDS |
| Fourth (lowest) | system bus | 68040 |

*"The 68040 is the lowest-priority device, but the arbiter does support bus parking, so the
average latency for 68040 bus access is minimized"* [4] §"Bus master priority scheme" p. 374 —
and the arbitration *"includes a degree of fairness that should keep devices from becoming
bus-starved"*, while *"the current bus master has complete control of both the system bus and the
I/O bus"* (a requesting SONIC waits for the 68040 to relinquish the bus) [4] p. 373. One more
documented behaviour: *"If a system bus master attempts to burst data to or from an I/O bus
slave, the Relayer ASIC will invoke a 68040 fake burst by asserting the 68040 /TBI signal"* [4]
p. 374 — the narrow I/O bus is bridged into 68040 burst semantics by the adapter.

### 5.4 Cache coherency and alternate masters

The 68040's cache snooping is present on the die but unused: *"The MC68040 has another new
feature called cache snooping. That feature is not used in the Macintosh Quadra 700 computer and
is not supported by the software. Devices that transfer data on the system bus, such as PDS bus
masters, must drive the snoop control pins to indicate no snooping"* [1] §"MC68040
microprocessor" p. 7, [2] p. 8. With the data cache in CopyBack mode the consequences are
software's to manage, and Apple states the rule the ROM itself follows: *"the ROM software uses
only pages marked uncacheable when setting up communication areas with alternate bus masters"*
[1] §"Cache management by the ROM" p. 41, [2] p. 44 — which is why SONIC descriptors and buffers,
NuBus DMA buffers and the SCSI pseudo-DMA path all live on uncacheable pages. The MMU map is
built to match the hardware: one ROM image and one I/O image, *"mapping one image of each address
space reduces the size of the MMU tables"* [1] §"Support for built-in MMU" p. 40.

### 5.5 Clocks

| Domain | Quadra 700/900 | Quadra 950 | Source |
|---|---|---|---|
| System bus / CPU | 25 MHz | 33.333 MHz (CPU 66.666 internal, 2X clock) | [1] p. 4, [2] p. 7, [3] p. 5 |
| I/O bus | 15.6672 MHz, asynchronous | 24.28416 MHz | [1] p. 10, [2] p. 10, [3] p. 7 |
| NuBus | 10 MHz, plus /CLK2X at 20 MHz for card-to-card 2X block transfers | same | [1] pp. 4, 23, [2] pp. 7, 26 |
| VIA timers | derived from Relayer's VIA clock output | — | [1] p. 10, [2] p. 10 |
| SONIC | 20 MHz-class part | 25 MHz version of the DP83932 | [sonic.md](../../hardware/sonic.md) §1.4 |
| SCSI | 53C96 at the system-bus rate class; TurboSCSI timing programmed per CPU speed | 33 MHz class | [ncr-53c96.md](../../hardware/scsi/ncr-53c96.md) §4.2, [3] §"Faster PDS" p. 6 |
| Video scan | DP8531 synthesizer, per mode | same, 80 ns VRAM | [dafb.md](dafb.md) §2.5, [3] p. 6 |
| RTC | 32.768 kHz (700: direct; towers: inside Caboose) | inside Caboose | [rtc.md](../../hardware/rtc.md), [2] p. 9 |

The VIA clock is Relayer's generated output (§2.2); the VIA timer rate that follows is the classic
Macintosh one. The 950's I/O bus speed-up is the reason for its faster SONIC part (§2.1) and
shorter I/O latencies [3] p. 7.

### 5.6 Reset, power and NuBus standby

Reset classes to distinguish: power-on (overlay armed, peripherals at power-on defaults), the
68040-visible CPU reset (overlay re-armed), and peripheral resets (SCSI bus, SWIM, SCC, NuBus)
which are not motherboard resets. The MCU overlay is disarmed by the first ROM-aperture access
and re-arms on reset (§2.9). The towers' power sequencing runs through Caboose — keyswitch,
*"the computer can be turned on from the keyboard and turned off by choosing the Shut Down menu
item"*, with the SECURE position disabling ADB and floppy and auto-starting on power application
[2] §"Keyswitch" p. 26 — and the Q900's supply keeps a trickle rail alive whenever the machine
is plugged in: *"+5V TRKL is a trickle supply that is available whenever the computer is plugged
in. That output provides power for the parameter RAM and the power-on logic, along with standby
power for the NuBus cards"* [2] §"Power supply" p. 24. The same standby reaches the slots as a
NuBus '90 feature: the Q900's connectors carry **STDBYPWR** (pin B25), *"low current at +5 V …
available on the new STDBYPWR pin when main power is off and the AC cord is plugged in"* [2]
Table 2-1 p. 27. NuBus power budgets differ sharply per machine: the 700 allows 15 W per slot or
30 W total with standard cards only; the 900 allows 19 W per slot, or two 25 W and three 15 W
cards, up to 95 W total, and takes oversized cards [1] Table 1-4 p. 26, [2] Table 1-4 p. 29,
[2] §"Oversized NuBus cards" p. 25.

## 6. Per-machine index

### 6.1 Macintosh Quadra 700 — [q700.md](q700.md)

The desktop member: a IIci-sized vertical case, MC68040 at 25 MHz, the Spike board. Its deltas
against this page: **two** NuBus slots (physical connectors for slots $D and $E, VIA2 PA1–PA3
unconnected) plus the PDS in line with slot $E [1] §"Expansion slots" p. 22; **two DRAM banks**
with 4 MB soldered as the fixed bank A [1] p. 11; **one** 53C96 shared by the internal and
external SCSI connectors [1] Table 1-4 p. 26; direct SCC, SWIM and RTC and a IIci-style ADB
[1] Table 1-4 p. 26; 512 KB of VRAM shipped in one of four banks [1] §"Video frame-buffer
controller IC: DAFB" p. 15; one NuBus-standard power budget. Gestalt machine type 22, SysEnvirons
type 20 [1] §"Gestalt and SysEnvirons values" p. 40.

### 6.2 Macintosh Quadra 900 — [q900.md](q900.md)

The tower: the Eclipse board in a floor-standing case with a 300 W supply [2] §"Summary of
hardware features" pp. 2–3. Deltas: **five** NuBus slots ($A–$E) with higher power and oversized
card support [2] §"NuBus slots" pp. 24–25; **four SIMM banks**, 4–64 MB [2] p. 11; **dual 53C96**
— internal and external SCSI buses, *"logically connected but electrically separate"*, with the
internal bus tuned to 5 MB/s [2] §"SCSI controller ICs" pp. 11, 13; IOP-based serial, floppy and
ADB; **Caboose** managing keyswitch, power, RTC, PRAM and sound [2] p. 9; 1 MB of VRAM shipped in
two banks [2] §"Video frame-buffer controller IC: DAFB" p. 14; the keyswitch and trickle-power
behavior of §5.6. Gestalt machine type 20, SysEnvirons type 18 [2] §"Gestalt and SysEnvirons
values" p. 44.

### 6.3 Macintosh Quadra 950 — [q950.md](q950.md)

The 950 is the 900's board at 33.333 MHz with a revised video path and its own ROM. Deltas: the
33.333 MHz system bus (CPU at 66.666 MHz internal) and a heat sink on the processor [3]
§"Faster processor clock" p. 5; the 24.28416 MHz I/O bus and the 25 MHz SONIC [3] p. 7; 80 ns VRAM
and the reprogrammed DAFB, adding **16 bits per pixel** on monitors the 900 could only drive at
8 bpp — up to 32,768 colors, selected as *"Thousands"* in the Monitors control panel [3]
§"Improved video on large monitors" p. 6; RAM documented at 4 or 16 MB per bank, 64 MB maximum
[3] §"Summary of features" p. 5; the DAFB version/test register reads the revised part
([dafb.md](dafb.md) §2.2) and the RAMDAC is the AC842a ([dafb.md](dafb.md) §2.4). The PDS is
identical to the 900's except for 33 MHz timing [3] §"Faster PDS" p. 6. Gestalt machine type 26
[3] §"Identifying the Macintosh Quadra 950" p. 8.

### 6.4 The Workgroup Server 95 derivative

The AppleWorkgroup Server 95 (1993) is a server packaging of the Quadra 950 board: the same 33.333
MHz 68040 tower, the same MCU/DAFB architecture, with a PDS card adding server features — a
second processor-direct cache/storage subsystem. The 950's ROM family serves it [6]. The card's
address map and register surface are not documented in any Apple source in evidence, and its
details are out of scope for this page (*reported*; see §7).

## 7. Open questions

1. **Orwell's bit assignment on silicon.** The §2.5 table is recovered from the boot ROM's
   programming, not from Apple or from hardware capture: no document, ROM listing or logic-analyzer
   trace in evidence states one MCU register bit. The bank-start fields are strongly corroborated
   by the observed split ($10/$20/$30 into 64 MB windows), but the speed, refresh and parity
   fields have only the consistency argument. Hardware readback per SIMM configuration is the
   closing experiment.
2. **The +$0A0–$0BF latch grouping.** Which strobe commits which staged field, and whether the
   strobes carry any address decoding below the longword, is recovered only from write ordering;
   the boot trace shows the addresses but no readback semantics exist.
3. **Overlay edge cases.** Whether a *write* to the $40000000 aperture triggers the overlay drop
   or only reads do; what the overlay ROM image aliases above the first megabyte at $00000000;
   and what a write to $00000000–$000FFFFF does while the overlay is armed — none of these is
   stated by Apple's single-sentence description [1] p. 11.
4. **The I/O mirror extent.** Apple's map repeats the 256 KiB I/O block through $53FFFFFF [4]
   p. 375; how many images physical boards actually decode, and whether every device shares the
   same mirror extent, is not established.
5. **YANCC's register file.** The control/status register's address is published ($50028000) and
   its error interrupt is in the level-7 mapping, but its width, bit layout, reset values and
   error-address capture are unknown — nothing in evidence reads or writes it except the ROM's
   error path.
6. **Relayer's watchdog timeout value**, and the per-device wait-state classes the I/O adapter
   inserts — the DCaD-era 44 µs/16 µs figures of other machines are not stated for this family.
7. **Open-bus behaviour of reserved I/O offsets** — bus error, floating bus, or a stable value —
   and the undecoded-read value inside the island generally.
8. **The bank-window behaviour above installed capacity**: aliasing from unconnected SIMM address
   lines, open bus, or bus error, per bank geometry — the ROM's sizing probes depend on it.
9. **Model-sense encoding.** The strap patterns that distinguish the Q700, Q900 and Q950 on VIA1
   port A are recovered from the ROM's identify code; the complete electrical encoding, including
   diagnostic-switch positions, is not documented anywhere in evidence.
10. **The towers' second SCSI lane detail.** The external 53C96's register stride and pseudo-DMA
    aperture position are recovered (the Apple-documented block is $5000F400–$5000F7FF [4]
    p. 375); the exact byte-lane arrangement inside the block is not verified.
11. **Caboose's exact silicon.** Apple defines the function [2] p. 9 and the surviving schematic
    carries Egret-protocol signal names [8]; which 68HC05-class part and firmware Apple shipped,
    and whether the two towers' parts differ, is not established.
12. **The 950's RAM ceiling.** The 950 note documents 4 or 16 MB per bank, 64 MB maximum [3] p. 5;
    later Apple specifications and shipped configurations acknowledge larger SIMM arrangements.
    Whether the difference is a documentation era, a board revision, or an MCU mask change is
    not established from the evidence here.  Both towers' ROMs size the board's full 256 MB
    (four banks of four 16 MB SIMMs).
13. **The Q700 "DAFB II" schematic label** against the functional revision split (700/900-class
    part versus the 950's revised part) — carried as an open question in
    [dafb.md](dafb.md) §6 and not restated here.
14. **NuBus '90 transaction limits**: which optional transactions YANCC actually implements
    beyond the documented feature list, block-size limits, and retry/fairness behaviour under
    master contention.
15. **The Workgroup Server 95's PDS card**: its address map, register surface and DMA/cache
    behaviour are undocumented in every Apple source in evidence.

## References

1. Apple Computer, Inc., *Developer Note: Macintosh Quadra 700*, Developer Technical Publications,
   1991 — §"Summary of major features" pp. 1–2; §"Design architecture" p. 4 (three buses, system
   bus ICs); §"MC68040 microprocessor" pp. 5–7 (cache snooping note p. 7); §"Custom ICs" pp. 7–8;
   §"I/O bus adapter ICs: JDB and Relayer" p. 10 (I/O bus clock, adapter functions, no
   programmable registers); §"Memory Control Unit" pp. 10–11 (ROM control: 4-Mbit devices, SIMM
   socket, the overlay; RAM control: banks, SIMM rules, bank registers); §"SCSI controller IC"
   p. 11; §"NuBus controller IC: YANCC" p. 12; Figure 1-2 p. 9 and Figure 1-3 p. 14 (address
   maps); §"Video frame-buffer controller IC: DAFB" pp. 15–16 (slot-$9 assignment, VRAM banks,
   Table 1-1); §"Sound ICs: DFAC, Enhanced ASC, and Sporty" pp. 17–18; Table 1-4 (700 vs 900
   comparison) p. 26; §"Expansion slots" p. 22; §"NuBus slots" and Table 2-1 pp. 23–24
   (NuBus '90 features, double-rate card-to-card only, -5.2 V pin warning); §"Processor-direct
   slot" p. 25 and Table 2-2/2-3 pp. 27–30 (PDS pinout, /MEMRESET, /MI.SLOT, /PDS.SLOT.E.EN);
   §"PDS card design considerations" p. 29 (slot-E decode rules, /TA//TEA, no timeout);
   §"Macintosh Quadra 700 Direct Slot interrupt handling" pp. 36–38 (Table 2-6 p. 37, Table 2-7
   p. 38, VIA1 software-interrupt and A/UX-enable bits, interrupt control PAL); §"Gestalt and
   SysEnvirons values" p. 40; §"Support for built-in MMU" p. 40; §"Cache management by the ROM"
   p. 41; §"Support for SCSI controller" p. 45 (the vectored SCSI module); §"Support for Memory
   Control Unit" p. 45; §"ROM memory map" p. 48.
2. Apple Computer, Inc., *Developer Note: Macintosh Quadra 900*, Developer Technical
   Publications, 1991 — §"Introduction" p. 2 (IOPs); §"Summary of hardware features" pp. 2–3;
   §"Design architecture" p. 7 (three buses, five system-bus ICs); §"Custom ICs" p. 9 (Caboose);
   §"I/O bus adapter ICs: JDB and Relayer" p. 10; §"Memory Control Unit" p. 11 (four banks,
   64 MB windows, bank registers, ROM control and overlay); §"SCSI controller ICs" pp. 11, 13
   (dual 53C96, electrical separation, 5 MB/s); §"NuBus controller IC: YANCC" and
   §"Video frame-buffer controller IC: DAFB" pp. 13–14; Figure 1-2 p. 12 and Figure 1-3 p. 14;
   §"Power supply" p. 24 (Table 1-3, +5V TRKL); §"Keyswitch" p. 26; Table 1-4 p. 29;
   §"Expansion slots" and §"NuBus slots" pp. 24–25 (five slots, higher power, oversized cards);
   §"NuBus '90 features" and Table 2-1 pp. 26–27 (STDBYPWR); §"PDS card specifications" p. 39
   (/PDS.SLOT.E.EN contract); §"Macintosh Quadra 900 Direct Slot interrupt handling" pp. 40–41
   (Table 2-6 p. 40, Table 2-7 p. 41); §"Gestalt and SysEnvirons values" p. 44.
3. Apple Computer, Inc., *Developer Note: Macintosh Quadra 950* (preliminary), Developer
   Technical Publications, 1992 — Chapter 1 p. 1 ("basically a higher-performance Macintosh
   Quadra 900"); §"Summary of features" p. 5 (33.333 MHz, four banks of 4 or 16 MB, IOPs, dual
   SCSI); §"Faster processor clock" and §"Processor power dissipation" p. 5 (2X clock, 66.666 MHz
   internal, heat sink); §"Faster PDS" p. 6; §"Faster video hardware" and §"Improved video on
   large monitors" p. 6 (80 ns VRAM, 16 bpp, Thousands); §"Faster I/O bus" p. 7 (24.28416 MHz,
   25 MHz SONIC); §"Identifying the Macintosh Quadra 950" p. 8 (Gestalt 26).
4. Apple Computer, Inc., *Designing Cards and Drivers for the Macintosh Family*, third edition,
   Addison-Wesley, 1992 — Part I §"NuBus expansion" p. 3 (Quadra-family slave block-transfer
   support); Part I §"Cache-coherency signals" p. 59 (protocol not implemented; /TM2
   Quadra-only); Chapter 5 Table 5-5 p. 113 (NuBus '90 signals on the Quadra-family connectors;
   the -5.2 V line warning); Part II Figure 1-6/1-7 pp. 26–27 (block diagrams); Part II
   §"Macintosh Quadra-family computers" p. 20 (the custom-IC list, Caboose 900-only); Part II
   §"RAM" p. 28 (contiguous stitching, burst reads and writes); Chapter 16 §"Design
   considerations for 68040 Direct Slot expansion cards" pp. 373–379: §"Bus master priority
   scheme" pp. 373–374 (Table 16-4, bus parking, the /TBI fake burst), §"Memory and I/O access
   for expansion cards" p. 374 (upper-RAM and slot-$E placement), Table 16-5 p. 375 (the 32-bit
   physical address spaces), §"Pseudoslot design guidelines" p. 376, Table 16-6/16-7 p. 378
   (VIA2 interrupt lines, interrupt mapping).
5. Macintosh Quadra 700/900 boot ROM, version `$420DBFF3` (October 1991; 1 MB big-endian image,
   also shipped in the PowerBook 140/170 family) — the Quadra 700 and Quadra 900 run this same
   image; the machine pages carry the identity consequences. Evidence used: the boot-time
   Orwell register-file access trace at $5000E000 (Quadra 700 configuration, power-on through
   memory sizing: the early first-longword reads from the overlay and again from the normal
   aperture; the halving-chain write burst at +$000–+$084; the +$0A0–$0BC strobes; the
   reprogramming bursts), and the ROM image's memory-sizing and machine-identify code from
   which the §2.5 bit assignment and the VIA1 model-sense reads are recovered. All *observed*
   sequence data in §2.7 comes from this trace.
6. Macintosh Quadra 950 boot ROM, version `$3DC27823` (March 1992) — the 950's separate image;
   its video driver carries the AC842a/x555 programming path (see [dafb.md](dafb.md) §2.4) and
   its identify code reads the 950's VIA1 model-sense straps (*observed*).
7. Apple Computer, Inc., Macintosh Quadra 700 main-logic-board schematic — the memory controller
   symbol labeled **"Orwell Memory Controller"**, the DRAM parity/test signal wiring, and the
   custom-IC signal wiring (the same drawing's DAFB symbol is discussed in
   [dafb.md](dafb.md) §1.2).
8. Apple Computer, Inc., Macintosh Quadra 900 production-validation (PVT) main-logic-board
   schematic — "Eclipse" project title block; the `EGRET_*` signal names on the Caboose nets; the
   dual-SCSI and VIA/interrupt wiring.
9. Apple Computer, Inc., *Guide to the Macintosh Family Hardware*, second edition, Addison-Wesley,
   1990 — the Macintosh II-family I/O conventions the Quadra I/O bus inherits (SCC on the upper
   byte of the data bus, even-addressed byte reads and odd-addressed byte writes) and the
   classic ROM-overlay and slot-space background.
