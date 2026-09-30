# Macintosh IIci

The Macintosh IIci is the senior board of the MDU family: a 25 MHz Motorola MC68030 with a 25 MHz
68882 floating-point coprocessor as standard equipment, 1–128 MB of 80 ns fast-page DRAM in two
interchangeable four-SIMM banks, the Macintosh II line's first built-in video, an optional parity
subsystem, a processor-direct cache connector, and three NuBus slots. Apple's own one-sentence
placing of the machine: "The Macintosh IIci is the first in a new series of Macintosh computers
compatible with the Macintosh II family (Macintosh II, Macintosh IIx, and Macintosh IIcx), and
offering improved performance and flexibility. The new architecture is based upon the Memory
Decode Unit (MDU) and RAM-based video chips (RBV). Key new features are a 25 MHz clock speed and
on-board video; most other features are the same as the Macintosh IIcx" [1] p. 3.

Everything the IIci shares with the IIsi — the MDU memory architecture, the RBV, the physical
memory map and its overlay, the interrupt, bus and clock tree, the device roster — is documented
once in the family doc ([mdu.md](mdu.md)) and cited here by section, never restated; the RBV's
registers, behaviour and programming model live on its device page
([rbv.md](rbv.md)) and are likewise cited. This page carries only what is unique to the IIci.

**Contents:**

1. [Identity](#1-identity) — what the machine is; how software recognizes it; spec table; the
   enclosure and its supply
2. [Deltas vs the family doc](#2-deltas-vs-the-family-doc) — the complete delta list against the
   MDU family
3. [Per-subsystem wiring](#3-per-subsystem-wiring) — clocks and timing; memory, SIMMs and the PGC;
   video; VIA1 and the RBV on this board; SCSI, serial, floppy, sound, clock and ADB; power, reset
   and the supply
4. [Expansion](#4-expansion) — the three NuBus slots; the cache connector; the diagnostic and
   emulator port; memory and ROM expansion
5. [Boot sequence summary](#5-boot-sequence-summary) — the family boot contract with this machine's
   branch points
6. [Open questions](#6-open-questions)
- [References](#references)

---

## 1. Identity

### 1.1 What the machine is

Introduced September 1989 [3] Table 1.4, the IIci is the first of the two MDU-family boards
(mdu.md §1.2) and the design the IIsi was later cost-reduced from. Apple narrates its ancestry as a
continuation of the IIcx's: the IIcx "began this evolution by taking the Macintosh IIx and removing
three NuBus slots and a floppy-disk drive", and "The Macintosh IIci project continues this
evolution with a completely new architecture, built around the Memory Decode Unit (MDU) and RAM
Based Video (RBV) chips. The NuChip was also modified, becoming the NuChip30, to work efficiently
with the 68030 bus" [1] p. 8. The IIci therefore carries the family's whole custom set on one
board — MDU, RBV, NuChip30, VDAC — plus the optional PGC parity IC and a real VIA1 behind a
discrete RTC and ADB transceiver (mdu.md §1.3, §4.2).

Apple's project name for the machine does not surface in the printed record of this evidence set.
The Hardware Overview sends developers to an Apple-internal "Atlantic and Pacific Theory of
Operation" for "how Mac IIci uses the MMU to implement its memory mapping" [3] p. 8 — by position
"Atlantic" names the IIci and "Pacific" the IIsi project (*inferred — unverified*; the mapping is
the family doc's conjecture, mdu.md §1.2). The codename "Aurora" sometimes attached to the IIci
appears in no document in this corpus (§6.1).

### 1.2 How software recognizes the machine

Two independent mechanisms identify a IIci to software, and a third (the ROM image) fixes the
machine's boot behaviour. None of the three uses a single ID register: there is no machine-ID
device on this board, only straps and ROM.

| Identity value | Value | Witness |
|---|---|---|
| VIA1 port A model-ID straps (CPU.ID3–0, on PA6/PA4/PA2/PA1) | 1, 0, 1, 1 — "The ROM code uses bits CPU.ID0–CPU.ID3 to identify which model it is running on" | [2] Tables 4-6 and 4-8 pp. 164–165 |
| CPU.ID2 with the parity option installed | 1 — the strap reads 1,0,1,1 without parity and 1,1,1,1 with it, so the ROM distinguishes the special-order parity model from the straps alone | [2] Table 4-8 note p. 165 |
| VIA1 port A bit 0 | BURNIN~ — a burn-in strap, not an ID bit | [3] pp. 63–64 |
| ROM image | 512 KB, header checksum longword `$368CADFE` at offset 0 | [5] (*observed* in the dumped image; no annotated IIci ROM disassembly exists in this evidence set) |
| ROM sockets | "DIP & SIMM" — the image ships in DIP ICs with a socket for a ROM SIMM | [1] Table 1-1 p. 9, p. 3 |
| ROM speed | 150 ns | [1] Table 1-1 p. 9 |
| Gestalt `mach` machine type | 11 — the machine-class dispatch constant of the shipped multi-machine m68k kernels, which route the IIci to the RBV register-offset class (`MACH_CLASSIIci`) | [6] (*observed* in the kernel headers; no Apple document prints the value) |
| RBV part | 343S1019-A | [3] p. 72, [rbv.md](rbv.md) §1.3 |
| Box flag / `SysEnvirons` machine type | not printed in any document of this evidence set | §6.2 |

The strap reading has one property worth stating for re-implementation: the four ID bits are the
*only* permanently-wired inputs of the port that carry model information, and they are compared by
mask, not by value — the IIfx shares ID3 and ID0 with the IIci and differs in ID2 and ID1, so an
implementation that reports, say, the IIfx pattern boots the wrong machine's drivers [2] Table 4-8
p. 165.

### 1.3 Spec table

| Parameter | Value |
|---|---|
| CPU | Motorola MC68030 at 25 MHz — "True 32-bit processor … internal 256-byte data and instruction caches as well as on-chip memory management. Burst reads to the on-chip cache are supported" [1] p. 4 |
| FPU | Motorola MC68882 at 25 MHz, fitted as standard [1] pp. 4, 9 |
| Coprocessor socket | none — the 68882 is standard equipment on the board, unlike the IIsi's adaptor-borne part (mdu.md §1.2) |
| Clock | 25 MHz CPUCLK, generated by the MDU (mdu.md §5.5); E clock 783.36 kHz; RBV-derived 15.6672 MHz and 3.672 MHz (rbv.md §1.6) |
| RAM | 1–128 MB, 80 ns fast-page-mode DRAM, two banks of four 30-pin SIMM sockets each; 256 KB, 1 MB, 4 MB or 16 MB SIMMs, four of one size per populated bank [1] pp. 3, 21, Table 1-1 p. 9 |
| ROM | 512 KB in DIP ICs plus a ROM SIMM socket; 150 ns devices; five CPU clocks per access, no burst (mdu.md §2.3) [1] Table 1-1 p. 9 |
| Video | built-in RBV video for the 12" B&W and 13" RGB (640 × 480, 1/2/4/8 bpp) and the 15" B&W Portrait (640 × 870, 1/2/4 bpp — no 8 bpp) monitors [1] pp. 3–5, [2] pp. 410–411; §3.3 below |
| NuBus | three slots, full-size 13" × 4" cards, full 32-bit address and data [1] p. 4; geographic slots $C–$E (mdu.md §3.4) |
| Cache | no cache on the board; a 120-pin processor-direct cache connector, 5 W card budget [1] pp. 3, 31, 39 |
| SCSI | NCR 53C80-compatible controller; one internal 50-pin connector for a 3.5" hard disk, one external DB-25 [1] pp. 4, 9, Table 1-1 p. 9 |
| Floppy | SWIM; one internal 1.4 MB Sony 3.5" drive, one external 800 KB or 1.4 MB drive; the external port does not support the 400 KB drive, though it supports 400 KB disks in an 800 KB drive [1] pp. 4, 8 |
| Serial | two 8-pin DIN ports, RS-232/RS-422 and AppleTalk, on a Zilog 8530 SCC [1] p. 4; listed as an "8 MHz 8530 SCC" [3] Table 1.4 (§6.7) |
| ADB | Apple Desktop Bus through a discrete transceiver chip behind VIA1 [1] Figure 1-2 p. 7, [3] Table 1.4 |
| Sound | ASC four-voice synthesis; two Sony custom amplifier/filter ICs; stereo external port, internal speaker [1] pp. 5, 7, [asc.md](../../hardware/asc.md) §6 |
| Clock/PRAM | discrete RTC behind VIA1: "Macintosh-compatible clock and parameter RAM with 7-year battery protection" [1] p. 4, [rtc.md](../../hardware/rtc.md) |
| Soft power | keyboard power-on and software power-off; the rear mechanical switch locks in the "on" position [1] p. 4, [2] p. 245 |
| Enclosure | the IIcx's horizontal three-slot case; 6.2 kg, 140 × 302 × 365 mm [2] Table A-8 p. 484 |
| Supply | shared with the IIcx: 90 W maximum continuous DC load, 108 W peak; three rails +5 V/+12 V/−12 V [2] Tables 6-20 and 6-21 pp. 261–262; §3.6 |

### 1.4 The enclosure

The IIci shares the IIcx's reduced horizontal enclosure (the Guide's front and interior views
cover both machines together [2] Figures 1-11 and 1-20), with room for one 3.5" internal hard disk
and one internal floppy drive, and an external connector for a second floppy [1] p. 4, [2] p. 144.
The back panel carries the two serial ports, the ADB port, the external floppy port, the external
SCSI DB-25, the stereo sound port and the DB-15 video connector — the last is the machine's most
visible novelty over the IIcx [1] Figure 1-2 p. 7, [2] Figure 1-13 p. 17. A hard-wired power
switch on the rear panel can be "locked in the 'on' position with a screwdriver or a coin", making
the machine restart itself after an AC interruption — the file-server feature of §3.6 [2] p. 245.
The optional programmer's switch pair (Reset and NMI buttons) fits the same case; its NMI button
is wired to the RBV, not to a VIA (§3.4).

## 2. Deltas vs the family doc

The complete list of what makes a IIci not an IIsi, each item expanded in §3 or §4:

| Delta | Against | Where |
|---|---|---|
| 25 MHz CPUCLK, and the whole timing set scaled to it | the IIsi's 20 MHz | mdu.md §5.5; §3.1 |
| Both banks socketed: four SIMM sockets per bank, either bank empty or four equal SIMMs; banks interchangeable | the IIsi's soldered bank A | mdu.md §2.1; §3.2 |
| 80 ns RAS / 20 ns CAS DRAM | the IIsi's 100/25 ns | mdu.md §2.3; §3.2 |
| 68882 FPU on the board as standard | none standard on the IIsi | mdu.md §1.2 |
| Optional PGC parity subsystem, 9-bit SIMMs, VIA1-controlled | nothing like it on the IIsi | mdu.md §4.2; §3.2, §3.4 |
| The 120-pin cache connector, with its diagnostic/emulator signals | no cache connector on the IIsi | mdu.md §2.4; §4.2, §4.3 |
| Discrete RTC and ADB transceiver behind VIA1 | the IIsi's Egret microcontroller owning ADB, RTC, PRAM, power and reset | mdu.md §4.2; §3.5, §3.6 |
| Programmer's switches: NMI button through the RBV at level 7 | no switches on the IIsi; keyboard-chord NMI instead | mdu.md §5.1; §3.4 |
| Three NuBus connectors, geographic $C–$E, NuChip30 on the board | one connector via adaptor kits, geographic $9, NuChip30 on the NuBus adaptor | mdu.md §3.4, §6.1–§6.2; §4.1 |
| Built-in video rides logical slot $B ($FB00 0000) | the IIsi's super-slot $E | [rbv.md](rbv.md) §3.5, §4.5; §3.3 |
| Sense code `010` reserved by Apple; no 512 × 384 mode in the printed decode | `010` runs the IIsi's 12" RGB mode | [rbv.md](rbv.md) §3.5; §3.3, §6.12 |
| No 8 bpp on the 640 × 870 Portrait format | (IIsi likewise, but the limitation is stated for the IIci in print) | [2] Table 12-3 p. 411; §3.3 |
| VIA1 carries the parity-enable and parity-error bits, and the machine-ID straps read 1,0,1,1 | the IIsi's VIA1 drops RTC/ADB bits to the Egret | [2] Tables 4-6, 4-8, 4-14; [3] pp. 63–64; §3.4 |
| Power: trickle-charged soft-power capacitor, lockable rear switch, 10-pin IIcx-pattern supply connector, /PFW pin | the Egret owns the IIsi's supply | mdu.md §2.5; §3.6 |
| Discrete 53C80 SCSI and 8530 SCC | the IIsi's Combo chip | mdu.md §4.1–§4.2; §3.5 |

## 3. Per-subsystem wiring

### 3.1 Clocks and bus timing

The clock tree is the family's (mdu.md §5.5) with the IIci's frequency: **CPUCLK is 25 MHz**,
generated by the MDU, and the MDU's own memory-cycle state machine is clocked by it on both edges
— the timing appendix's drawings are "referenced to the CPUCLK" with "both the rising and falling
edges of the CPUCLK … used to change states" [1] p. 63. The clock is exported to expansion on one
pin: **CPUCLK appears at cache-connector pin C38** [1] Table 4-4 p. 37, and the motherboard drives
only a CMOS input on it — 10 µA high and low, 15 pF, against the two-74LS-load budget of most
other signals [1] Table 4-3 pp. 34–36. The E clock (783.36 kHz, MDU-generated) and the RBV's
15.6672 MHz and 3.672 MHz outputs are the family's (mdu.md §5.5, [rbv.md](rbv.md) §1.6); the video
dot clocks (30.2400 MHz and 57.2832 MHz) are the RBV's, selected by monitor sense
([rbv.md](rbv.md) §3.5–§3.6).

The cycle timings the 25 MHz clock paces are the family's (mdu.md §2.3) — 5-clock burst-lead plus
three 2-clock beats from RAM, 5-clock random read, 4-clock random write, 6-clock refresh every
15.6 µs, 5-clock ROM access with no burst — and the figures are drawn for this machine's 80 ns
DRAM [1] Appendix A pp. 63–65. The IIci-specific performance number is the Guide's: burst plus fast
page mode gives "a maximum RAM access rate of 36.36 MB per second", true always for bank B, and for
bank A only "when using a NuBus video card and not using the built-in video circuits"; with a
cache card installed the maximum rate rises to 50 MB per second and is "largely unaffected by the
built-in video circuits" [2] p. 195. Why the larger SIMMs belong in bank B is the same fact read
backwards: the processor has immediate access to bank B at all times, so the majority of accesses
should land there [2] p. 195.

### 3.2 Memory: SIMM banks and the PGC

#### 3.2.1 Population rules

Both banks are socketed and interchangeable: "The amount of motherboard RAM is changed by
installing four of the same size SIMMs into either bank", and "For best performance with on-board
video, put the smaller SIMMs in bank A" [1] p. 21, Figure 3-1 note 1 p. 22. A bank holds nothing
or four SIMMs of one size — 256 KB, 1 MB, 4 MB or 16 MB — so a populated bank is 1, 4, 16 or 64 MB
and the machine spans 1 MB (one bank of 256 KB SIMMs) to 128 MB (both banks of 16 MB SIMMs) [1]
p. 21. The 256 KB SIMM is built from 256K×4 fast-page-mode parts (1 Mbit technology), and 4 Mbit
DRAM is supported with "16 Mbit if it remains compatible with the 4 Mbit DRAM" [1] p. 3, Figure 3-1
note 3 p. 22. The wrap behaviour that sizes the banks, and the fixed 64 MB bank windows, are the
family's (mdu.md §2.1, §3.6). Bank A must be populated for on-board video [1] p. 21.

The standard machine shipped with 8-bit SIMMs: "although the Macintosh IIci supports parity
checking, the standard machine configuration will be shipped with eight-bit DRAM SIMMs. For
parity, special units with the PGC and nine-bit DRAM SIMMs must be ordered" [1] p. 21 — and the
parity feature "is available in the Macintosh IIci only by special order; it is not available as
[an] upgrade" [2] p. 172.

#### 3.2.2 SIMM pinout

The SIMM is a 30-pin module with bypass capacitors [1] p. 24; the pinout, with the MDU's
row/column address mapping to the processor's address lines, from Tables 3-2 and 3-3 [1]
pp. 26–27:

| Pin | 8-bit SIMM | Processor bus | 9-bit parity SIMM |
|---|---|---|---|
| 1 | +5 V | +5 V | +5 V |
| 2 | CAS~ | one of CASLL~, CASLM~, CASUM~, CASUU~ | CAS~ (same) |
| 3 | DQ0 | D0, D8, D16, or D24 | DQ0 (same lanes) |
| 4 | RA0 | A6 row / A2 column | RA0 (same) |
| 5 | RA1 | A7 / A3 | RA1 (same) |
| 6 | DQ1 | D1, D9, D17, or D25 | DQ1 (same) |
| 7 | RA2 | A8 / A4 | RA2 (same) |
| 8 | RA3 | A9 / A5 | RA3 (same) |
| 9 | GND | GND | GND |
| 10 | DQ2 | D2, D10, D18, or D26 | DQ2 (same) |
| 11 | RA4 | A11 / A10 | RA4 (same) |
| 12 | RA5 | A13 / A12 | RA5 (same) |
| 13 | DQ3 | D3, D11, D19, or D27 | DQ3 (same) |
| 14 | RA6 | A15 / A14 | RA6 (same) |
| 15 | RA7 | A17 / A16 | RA7 (same) |
| 16 | DQ4 | D4, D12, D20, or D28 | DQ4 (same) |
| 17 | RA8 | A19 / A18 | RA8 (same) |
| 18 | RA9 | A21 / A20 | RA9 (same) |
| 19 | RA10 | A23 / A22 | RA10 (same) |
| 20 | DQ5 | D5, D13, D21, or D29 | DQ5 (same) |
| 21 | WE~ | RAMRW~ | WE~ (same) |
| 22 | +5 V | +5 V | +5 V |
| 23 | DQ6 | D6, D14, D22, or D30 | DQ6 (same) |
| 24 | RA11 | A24 / A25 | RA11 (same) |
| 25 | DQ7 | D7, D15, D23, or D31 | DQ7 (same) |
| 26 | n.c. | — | **PDO** — parity data out, PDO0, PDO1, PDO2, or PDO3 |
| 27 | RAS~ | RAS0~ (bank A) or RAS1~ (bank B) | RAS~ (same) |
| 28 | pull-up to +5 V | — | **PCAS~** — the parity byte's CAS strobe (same four forms) |
| 29 | n.c. | — | **PD** — parity data in, PD0, PD1, PD2, or PD3 |
| 30 | +5 V | +5 V | +5 V |

Three facts fall out of the table and are load-bearing for re-implementation. Each SIMM is one
byte wide and each of its DQ pins serves one of the four byte lanes D0–D7, D8–D15, D16–D23 or
D24–D31 — the SIMM's *position* within the bank selects the lane, which is why a bank takes four
modules and why the four CAS strobes (one per lane) land one each on the four modules' pin 2. The
MDU's row/column multiplexing skips address bits non-uniformly (row A6, A7, A8, A9, A11, A13, A15,
A17, A19, A21, A23, A24; column A2–A5, A10, A12, A14, A16, A18, A20, A22, A25), and the same
12-bit RA bus is shared by row and column with different mappings per phase — the pin table is the
only printed record of the mapping. And RAS is per *bank* (RAS0~/RAS1~ on pin 27) while CAS is per
*byte lane*, matching the two-bank, four-lane DRAM structure of mdu.md §2.1.

#### 3.2.3 The PGC and parity

Parity is generated and checked by the optional **Parity Generator and Checker (PGC)** chip
[1] pp. 27–28, [2] p. 222, with the ninth DRAM bit carried on the parity SIMMs' PDO/PD/PCAS~
wiring of the table above. Its contract:

- **Write path.** "Parity is always written to the parity bit if the PGC is present. If the bit is
  not physically present on the SIMM module, it is simply ignored — a problem only exists if
  parity is read from the bit (i.e., parity is enabled) when the bit is not present (i.e.,
  eight-bit DRAM SIMMs are in use)" [1] p. 28. Each byte written gets a generated parity bit
  stored in the ninth bit for that byte [2] p. 172.
- **Read path.** On every RAM read the PGC regenerates each byte's parity and compares it with the
  stored bit [1] p. 27, [2] p. 222.
- **Enable.** Checking starts disabled; startup code enables it only if the PGC *and* parity SIMMs
  are found, and only if *all* installed SIMMs are 9-bit [1] pp. 21, 28. The control bit is
  **VIA1 Data register B bit 6, /Par.En — 0 = parity checking enabled** [2] Table 4-14 p. 171.
- **Error.** A mismatch with parity enabled asserts two PGC outputs: "/NMI, which interrupts the
  main processor, and /PARERR, which indicates a parity error" [2] p. 172 — the IIci's parity
  circuits can therefore raise a level-7 interrupt [2] ch. 3 p. 100. The service routine reads
  VIA1 Data register B **bit 7, /Par.Err**, an input from the PGC, to tell a parity NMI from any
  other level-7 cause, and clears the PGC's outputs by setting /Par.En to 1 [2] pp. 171–172.
- **Startup obligation.** "Before reading RAM with parity enabled, the system software must set
  the parity bits properly by writing using normal parity to all available RAM" [2] p. 222 —
  RAM whose ninth bits were never written (or were left stale) would otherwise parity-fault
  immediately.
- **User-visible failure.** A detected parity error tells the user "A Memory Parity Error Has
  Occurred" and requires a reboot [1] p. 28.
- **Scope.** Parity errors are not generated for writes, for accesses outside the DRAM physical
  space $0000 0000–$07FF FFFF, or while checking is disabled [1] Figure C-1 note 3 p. 77. The
  cache-timing appendix adds the subtle interaction: in a parity IIci a parity error can occur if
  STERM~ is low at a rising CPU-clock edge unless valid DRAM data is present at the next falling
  edge, or CACHE is high at the next falling edge, or CACHE was high at the previous falling edge
  — the rule that keeps a cache-card hit from masquerading as a DRAM read with bad parity [1]
  p. 77, Figure C-3 note 2 p. 79.

The PGC's own specification — Apple's reference list names a *PGC User Manual* (Michael Dhuey)
[3] p. 72 — is not in this evidence set; the printed contracts above are everything the corpus
gives the part (§6.6).

### 3.3 On-board video

The IIci is the family's video machine in the fullest sense: the RBV scans bank A while the
processor works bank B, and the OS presents the buffer as a NuBus-style video device. The fetch
engine, the FIFO protocol, the monitor-sense scheme, the timing tables and the depth-selection
programming model are the RBV page's and are not restated here ([rbv.md](rbv.md) §3.4–§3.6,
§4.5); what follows is the IIci's wiring.

- **Modes.** Screen sizes 640 × 480 and 640 × 870; 1, 2, 4 and 8 bpp on 640 × 480, 1, 2 and 4 bpp
  on 640 × 870 — "The built-in video circuits on the Macintosh IIci do not support 8 bpp on the
  portrait display" [2] pp. 410–411, Table 12-3 p. 411.
- **Buffer placement and sizing.** The frame buffer starts at physical $0000 0000 in bank A and
  takes exactly the screen's needs: "The RBV will require only the amount of memory to hold the
  contents of the screen" [1] p. 23. The MDU allocates screen-buffer memory in 32 KB increments —
  64 KB for 1 bpp on a 640 × 480 monitor, 320 KB for 8 bpp colour [2] note p. 411. Software
  reserves the maximum intended depth at startup and may reuse the surplus at smaller depths
  [1] p. 23.
- **Logical mapping.** The buffer maps to logical $FB00 0000 — "That address space was chosen
  because it is the same as the address space used by expansion slot $B in the six-slot models of
  the Macintosh II family" — and the machine "can share common system ROM with the other Macintosh
  models that use the MC68030 processor" precisely because the built-in video impersonates a slot
  card [2] p. 412. The OS also reaches it as pseudo-slot $0 [2] p. 410.
- **Contention.** Only bank-A CPU accesses wait for video fetch; the numbers are the Guide's:
  video consumes 6% (1 bpp, 640 × 480) to 64% (8 bpp, 640 × 480) of bank-A bandwidth, and 13% to
  65% on the Portrait formats [2] Table 12-4 p. 413, with the resulting machine rates at §3.1. The
  fetch mechanics, and the RBV's precedence over the processor inside bank A, are
  [rbv.md](rbv.md) §3.4 and §3.7.
- **Monitor sense.** The IIci's MON.ID decode is [rbv.md](rbv.md) §3.5's first table: codes `010`
  and `101` are "RESERVED for use by Apple" on this machine [1] Table 5-1 p. 45 — the IIsi later
  runs its 12" RGB mode on `010` ([rbv.md](rbv.md) §5). With an unsupported or absent monitor, or
  when the display lives on a NuBus card instead, "the system software switches the built-in video
  circuits off" [2] p. 420; the halted output waveforms are specified ([rbv.md](rbv.md) §3.5).
- **The connector and cables.** A DB-15 on the back panel, "the same as the one on the Macintosh II
  Video Card and has the same signal assignments" [2] p. 419 — pins 1–15 carry red/green/blue
  video and grounds, composite sync, the three sense lines, vertical sync and horizontal sync
  [1] Table 5-3 p. 53. The 12" B&W and 13" RGB monitors connect with a pin-for-pin DB-15 cable;
  the 15" Portrait monitor uses a D-25 at its end, cross-connected per the IIci's Portrait cable
  table — composite sync is unused for Portrait-class monitors because they take separate
  HSYNC~/VSYNC~, and CSYNC.GND shares its DB-15 pin with VSYNC.GND [1] pp. 52–54, Table 5-4 p. 54.
- **Analog output.** The RBV's 8-bit pixel stream feeds the **Brooktree Bt478** CLUT DAC — "two
  parts of a single integrated circuit" — which indexes its 256-entry lookup table and drives
  three 8-bit DACs whose outputs "provide RS-343-A-compatible RGB video signals to the video
  connector"; the sync signals come directly from the RBV, not through the Bt478 [2] p. 418,
  [1] Figure 1-2 p. 7 ("VDAC 478 D/A & Color Lookup"). The part sits in the family's I/O island
  at the VDAC window (mdu.md §3.3).

### 3.4 VIA1 and the RBV on this board

#### 3.4.1 VIA1 wiring

VIA1 is a real 6523-class VIA ([via.md](../../hardware/via.md)) carrying the II-family timer and
serial-port roles (mdu.md §5.1) plus the IIci's own machine straps and parity bits. The complete
port wiring, from the Hardware Overview's IIci column [3] pp. 63–64 with the Guide's register
tables [2] Tables 4-6 and 4-14 pp. 164–171:

| Pin | Signal | Meaning |
|---|---|---|
| PA0 | BURNIN~ | burn-in strap [3] pp. 63–64 |
| PA1 | CPU.ID0 | model-identity strap bit 0 |
| PA2 | CPU.ID1 | model-identity strap bit 1 |
| PA3 | vSync (MODEM) | "1 = synchronous modem support, channel A" [2] Table 4-6 p. 164 |
| PA4 | CPU.ID2 | model-identity strap bit 2 — 0, or 1 on the parity model [2] Table 4-8 p. 165 |
| PA5 | vHeadSel (SEL) | floppy state-control line [2] Table 4-6 p. 164 |
| PA6 | CPU.ID3 | model-identity strap bit 3 |
| PA7 | vSCCWrReq | "0 = SCC Wait/Request, channel A or B" [2] Table 4-6 p. 164 |
| PB0–PB2 | rtcData, rtcCLK, rTCEnb | real-time clock serial interface [2] Table 4-14 p. 171, [rtc.md](../../hardware/rtc.md) |
| PB3 | vFDBInt (/ADB.INT~) | ADB interrupt pending [2] Table 4-14 p. 171 |
| PB4–PB5 | vFDesk1, vFDesk2 (ADB.ST0/ST1) | ADB transceiver state bits [2] Table 4-14 p. 171, [adb.md](../../hardware/adb.md) |
| PB6 | /Par.En | 0 = parity generation and checking enabled — IIci only [2] Table 4-14 p. 171 |
| PB7 | /Par.Err | input from the PGC; 0 = parity error — IIci only [2] Table 4-14 p. 171 |
| CA1 | /VBLK | 60.15 Hz vertical-blank request [3] pp. 63–64 |
| CA2 | RTC 1 Hz | one-second tick from the RTC [3] pp. 63–64 |
| CB1–CB2 | ADB.CLK, ADB.DATA | ADB transceiver lines [3] pp. 63–64 |

Bit 7 of port B is the classic sound-enable position on one-VIA Macintoshes; on the IIci it is an
input instead, "used as an output only to maintain compatibility" elsewhere in the II family [2]
p. 172 note.

#### 3.4.2 The RBV's IIci-specific wiring

The RBV's register file, interrupt concentration, slot-interrupt machinery and video control are
[rbv.md](rbv.md) §2–§4 and are not restated. The IIci-specific items:

- The control byte's cache-enable and cache-flush bits and the **PAR.TEST~** bit (bit 7, the
  IIci's "signal used for testing the parity circuits") are [rbv.md](rbv.md) §2.3; the
  power-off bit is the IIci's real soft-power path ([rbv.md](rbv.md) §3.9, §3.6 below).
- The slot-interrupt status register's six slot positions answer the three physical connectors at
  the II-family bit-to-slot positions of slots $C, $D and $E, with the built-in video's
  slot-style interrupt riding the same register family ([rbv.md](rbv.md) §2.4; the exact position
  of the video bit and the conflicting printed labels for bits 6–7 are [rbv.md](rbv.md) §6,
  item 4, and §6.4 below).
- The Interrupt Flag register's bit 2, **/EXP.IRQ, is "Macintosh IIci only"** in the Guide's
  table [2] Table 4-27 p. 186 — an expansion interrupt input that the IIsi does not carry; what
  it is wired to is not printed ([rbv.md](rbv.md) §2.5 and §6.10; §6.5 below).
- The VIA2-emulation's extra outputs are the IIci's own: "It provides two signals used to enable
  and to flush the optional RAM cache. It provides a signal used for testing the parity circuits"
  [2] p. 157.
- Interrupt levels are the family's (mdu.md §5.1, [rbv.md](rbv.md) §3.1): VIA1 at level 1, the RBV's
  VIA2 emulation at level 2, the SCC at level 4, NMI at level 7 — with this machine's two
  level-7 sources: the programmer's switch, "connected directly to the general-logic IC — … RBV
  in the Macintosh IIci", generating level-7 interrupts that the 68030's priority mask cannot
  inhibit [2] ch. 3 p. 102; and the parity circuits' /NMI (§3.2.3).
- The built-in video's vertical blanking arrives as a slot-style interrupt "handled like a NuBus
  interrupt" [2] p. 62 — and Apple's developer tip marks the distinction that matters for
  multimedia software: the 60.15 Hz VIA1 tick is a compatibility timer, while the actual
  vertical-blanking edge is the slot interrupt from the video circuits; software that must sync
  to the real blank must use the latter [2] p. 172.

### 3.5 SCSI, serial, floppy, sound, clock and ADB

All five subsystems are the family's roster entries (mdu.md §4.1) with IIci-specific wiring notes
only:

- **SCSI** is the discrete NCR 53C80-compatible controller ([ncr-5380.md](../../hardware/scsi/ncr-5380.md)),
  one internal 50-pin connector and one external DB-25 [1] Table 1-1 p. 9. Its IRQ and DRQ
  interrupt outputs are RBV flag bits — "both the IRQ and DRQ signals from the 5380 are stored in
  the Interrupt Flags register in the VIA2 portion of the RBV" [2] p. 393 — and the MDU performs
  the pseudo-DMA handshake (mdu.md §2.2, §3.8).
- **Serial** is a Zilog 8530 SCC ([scc.md](../../hardware/scc.md)) on the two 8-pin DIN ports [1]
  p. 4. Its interrupt output goes "directly to the general-logic IC — … RBV in the Macintosh IIci"
  [2] ch. 3 p. 100, at level 4 (mdu.md §5.1); its Wait/Request output is readable at VIA1 PA7
  (§3.4.1). The Hardware Overview rates the part "8 MHz 8530 SCC" against the 4 MHz parts of the
  II/IIx/IIcx [3] Table 1.4 — the only such claim in the corpus, and unexplained by the RBV's
  3.672 MHz SCC rate clock ([rbv.md](rbv.md) §1.6, §6.7 below).
- **Floppy** is a SWIM ([iwm-floppy.md](../../hardware/iwm-floppy.md)) driving one internal 1.4 MB
  SuperDrive and one external 800 KB or 1.4 MB drive; the external port "does not support the
  400K floppy drive. It does support 400K disks used in the 800K drive" [1] p. 8. The floppy
  head-select line is VIA1 PA5 (§3.4.1).
- **Sound** is the ASC ([asc.md](../../hardware/asc.md)) with two Sony custom amplifier/filter ICs
  — the Guide's ASC circuit diagram covers the II, IIx, IIcx, IIci and IIfx together [1] p. 5,
  [2] Figure 13-5 p. 440. The ASC's interrupt is RBV IFR bit 4 ([asc.md](../../hardware/asc.md)
  §6); the external-jack sense is the RBV control byte's SND.EXT~ bit ([rbv.md](rbv.md) §2.3),
  which is what lets the Sound Manager choose mono-internal versus stereo-external [2]
  pp. 156–157.
- **RTC and PRAM** are the discrete II-family custom clock chip behind VIA1 PB0–PB2
  ([rtc.md](../../hardware/rtc.md)): 32.768 kHz oscillator, 256 bytes of battery-backed parameter
  RAM of which twenty bytes are directly addressable for compatibility with the older part, and a
  one-second interrupt into VIA1 CA2 [3] ch. 11 p. 58, [1] p. 4.
- **ADB** runs through the discrete transceiver chip wired to VIA1 CB1/CB2 and PB3–PB5
  ([adb.md](../../hardware/adb.md)); the keyboard's power-on key reaches the soft-power circuit
  through the ADB connector without involving the transceiver ([2] p. 77, §3.6).

### 3.6 Power, reset and the supply

The soft-power design is the family's trickle-capacitor scheme (mdu.md §2.5), and the IIci is the
documented instance of it: "The capacitor in the Macintosh II and Macintosh IIx is kept charged
by two 3-volt lithium batteries; in the Macintosh IIcx and Macintosh IIci, it is kept charged by
a trickle current from the power supply" — so the IIci keeps its soft power with no batteries,
through the supply's +5V.TRKL pin [2] p. 244. Pressing the keyboard power key (or the rear
switch) switches the supply on within 2 seconds [2] p. 244. Shutdown is software-driven: the OS
completes all pending activity, then the RBV asserts /POWEROFF ([rbv.md](rbv.md) §3.9) and "This
signal causes the power supply to switch off after 2 ms" [2] p. 245. The lockable rear switch is
the file-server feature: locked on, the machine "will turn itself back on after a power
interruption", and a Shut Down from the Finder then causes a *restart* rather than power-off [2]
p. 245. The dev note adds that "Slot access to power control allows power to be controlled by
NuBus cards" [1] p. 4 — the only printed statement that a slot can reach the power path on this
machine, without naming the pin or the mechanism (§6.9).

Reset is the II-family flow the family doc carries (mdu.md §2.5, §5.6): a Sony sound IC monitors
the board voltages and holds /RESET asserted until 0.25 s after they stabilize; /RESET reaches the
CPU, the general logic ICs, all internal devices and every expansion slot, and the CPU then
fetches its reset vectors from $0000 0000 [2] p. 244. There is no separate Test/MDU-reset
sequence on this machine — that refinement belongs to the IIsi's Egret (mdu.md §2.5, §7 item 14).

The supply itself is the IIcx's. The logic-board connector is a 10-pin assignment — +12 V, three
+5 V, three GND, −12 V, **/PFW** (power-fail warning) and **+5V.TRKL** (the trickle feed of the
power-on circuit) [2] Table 6-5 p. 250. Its specified envelope:

| Parameter | Value |
|---|---|
| AC input | 85–135 V rms and 170–270 V rms; 47–63 Hz; 300 V rms surge for 100 ms; 40 A peak inrush; 0–6 kV line-transient immunity; 20 ms line-dropout immunity [2] Table 6-10 p. 256 |
| Output limits | +5 V: 4.9–5.2 V; +12 V: 11.5–12.8 V; −12 V: −13.2 to −10.8 V [2] Table 6-20 p. 261 |
| DC loads | minimum 10.5 W (2 A at +5 V); maximum 90 W (12 A / 1.5 A / 1 A); peak 108 W for 15 s at 10% duty, +12 V may drop to 11 V [2] Table 6-21 p. 262 |
| Ripple and noise | 20/40 mV pp at +5 V, 30/50 mV pp at ±12 V (line ripple / switching noise) [2] Table 6-22 p. 262 |

Per-slot and per-card power budgets are §4.1 and §4.2.

## 4. Expansion

### 4.1 The three NuBus slots

The NuBus interface "remains the same as the Macintosh IIcx, except that the slots are numbered 4
through 6 and mapped to geographic addresses $C through $E. On the Macintosh IIcx, they were
numbered 1 through 3 and mapped to geographic addresses $9 through $B. This should not matter to
the cards" [1] p. 59 — the slot-strap coding, the standard/super slot windows, the $F0xx
processor exclusion and the NuBus-to-main-board translations are the family's (mdu.md §3.4, with
the strap table). The connectors take "full size cards (13" x 4")" with "full 32 bit address and
data" [1] p. 4, and the NuChip30 that fronts them is on the main board [1] p. 8, Figure 1-2 p. 7
(the IIsi moves the same part onto its adaptor card, mdu.md §6.2). Card power is budgeted per
slot: 2.0 A at +5 V, 0.175 A at +12 V and 0.150 A at −12 V [1] Table 4-5 p. 39. The 24-bit
compatibility map names the three physical slots $C/$D/$E and marks geographic $9–$B "not on
IIci" [1] Table 2-1 p. 16 (mdu.md §3.5). Apple's expansion ceiling for the machine counts NuBus
memory cards: "Over 800 MB of expansion RAM is possible in NuBus slots" [1] p. 3.

Card-to-card NuBus transfers never reach the CPU bus, which is why the OS marks NuBus space
non-cacheable ([1] p. 34, mdu.md §5.3); a NuBus master's own RAM cycles run through the NuChip30's
CPU-bus mastership, drawn in the cache-timing appendix as /BR, /BG, /BGACK, the DRAM access, then
"bus release by NuBus (waits for NuBus ACK)" [1] Figure C-2 p. 78 (mdu.md §5.3).

### 4.2 The cache connector

The cache connector is the IIci's second expansion path and the family's most unusual one; its
bus-level contract — the CACHE signal that disables the MDU, the BGACK~ pull-up rule, the
card-decoded data/tag windows at $5200 0000–$527F FFFF and $5280 0000–$52FF FFFF, the ROM traps
off HWPriv — is the family doc's (mdu.md §2.4, §3.7) and is not restated. What is IIci-specific:

- **Physical.** A 120-pin Euro-DIN connector, "the same connector as the SE/30 provides", but
  "SE/30 cards are not compatible with the Macintosh IIci" — the four printed reasons: a
  different form factor (the IIci case), no back-panel cutout for external I/O, 25 MHz rather than
  16 MHz operation, and 5 W at +5 V only [1] p. 31. The warning is unequivocal: mismatched cards
  "may damage both the computer and the card" [1] p. 31, and Apple "strongly suggests the use of
  synchronous logic (clocked by CPUCLK) on a cache card" [1] p. 31.
- **Signal set.** The connector carries the 68030 bus — A0–A31, D0–D31, AS~/DS~/R/W~/SIZ0–1,
  FC0–2, RMC~, /BERR, /HALT, the arbitration pair BG~ and BGACK~ (the 68030's own /BR output is
  not on the cache pinout; it appears only on the diagnostic pins of §4.3), STERM~ — plus the cache
  group CBREQ~/CBACK~ (the 68030's burst handshake), CIOUT~ (the processor's own cache-inhibit
  output), **CACHE** (the one output toward the MDU), CENABLE~/CFLUSH~ (the RBV's strobes,
  [rbv.md](rbv.md) §3.8), RESET~ and CPUCLK [1] Table 4-3 pp. 34–36. Drive budgets: most signals
  drive two 74LS inputs; RESET~, D24–D31 and FC0–FC2 drive only one; CPUCLK drives only a CMOS
  input [1] p. 34.
- **Mechanical.** Maximum card 3.0 in high × 6.1 in long; thickness 0.062 ± 0.0075 in; warpage
  within 0.10 in; no components or traces in the top 0.150 in on either side; no active
  components on the back side, and nothing there extending more than 0.10 in; component height
  0.40 in on the front (power-supply) side [1] pp. 39–42.
- **Power.** "5 watts of power are allocated at +5 volts only" — 1.0 A at +5 V, +12 V and −12 V
  "not available" [1] Table 4-5 p. 39.
- **Behaviour.** A cache card is a *physical* cache with no access to the 68030's on-chip MMU, so
  coherency is not a problem and "there should be no reason to flush the cache except when
  enabling the cache" [1] p. 32. The MMU marks NuBus slot space and all I/O space non-cacheable,
  and 68030 accesses to those locations are never cached [1] p. 32. A hit costs the processor
  "just two clock cycles"; a miss costs "an additional seven clock cycles to retry the read
  operation", during which the card also fills itself from the RAM data as the MDU supplies it
  [2] p. 68. Cache fills ride the RAM burst: "read operations from main RAM use burst mode, which
  takes five clock cycles for the first read and two clock cycles each for three subsequent read
  operations", filling a four-longword chunk aligned on a 4-longword boundary [2] p. 225. On a
  write, a card may update its data RAM in parallel with main memory or instead invalidate its
  tag's validity bit [2] pp. 225–226. The pay-off is Apple's headline number: "High-speed RAM on
  the cache card can improve the performance of the Macintosh IIci by as much as 50 percent,
  depending on the application and on the pixel size used by the built-in video display" [2]
  p. 223, and with a card the maximum RAM access rate is 50 MB per second (§3.1). RESET~ returns
  the cache disabled ([rbv.md](rbv.md) §3.8).

### 4.3 The diagnostic and emulator port

The cache connector doubles as the machine's hardware debugging port, on pins the printed pinout
marks "n.c." [1] pp. 69–73, [4] ch. 23 pp. 519–521. The signals "are provided only for debugging
and emulator support", carry "Apple's internal use" caveat, and "may not be supported in future
implementations of the cache connector" [1] p. 69:

| Signal | Meaning on the IIci |
|---|---|
| /ROMOE | ROM output enable — visibility of the MDU's ROM strobe [1] Table B-1 p. 70 |
| /DSACK0~, /DSACK1~ | the MDU's asynchronous acknowledges, for cycles the synchronous STERM~ path does not cover [1] Table B-1 p. 70 |
| /IPL0~–/IPL2~ | the processor interrupt priority lines [1] Table B-1 p. 70 |
| /BR | bus request — a master's request as the CPU sees it [1] Table B-1 p. 70 |
| CPUDIS | "used to disable the MC68030 on the motherboard and render its outputs high-impedance. An emulator in the cache connector may assert CPUDIS and, after waiting for the end of the current bus cycle, may drive all signals" [1] p. 69 |

The parenthesized entries in the appendix's table — the address bus, function codes, AS~, R/W~,
SIZ0–1, DS~, RMC~ — are "usually driven by the MC68030, but are rendered high-impedance by the
MC68030 after granting the bus to a DMA requestor", and may then be driven by the card [1] p. 69.
Apple's framing is restrictive: "while the connector is capable of other functionality, Apple
intends to support its use for RAM cache products only" [1] p. 69.

### 4.4 Memory and ROM expansion

Memory expansion is entirely by SIMMs under the §3.2.1 rules; parity expansion is not possible
after purchase ([2] p. 172 note). ROM expansion is a IIci novelty over the IIcx: "a ROM SIMM
allows future ROM revision in the field" [1] p. 3, and the ROM subsystem ships as DIP ICs on the
logic board "and one set on motherboard" in the block diagram's phrasing, with the SIMM socket
beside them [1] Figure 1-2 p. 7, Table 1-1 p. 9. The Guide confirms the arrangement — the IIci's
ROM ICs are mounted on the main logic board, and "The Macintosh IIcx and Macintosh IIci also
have a connector for an optional SIMM" — and states the software consequence: the SE/30, IIcx and
IIx share identical ROM code, while "Each of the other Macintosh computers has ROM code unique to
that computer" [2] ch. 2 pp. 68–69. Each ROM read makes a longword available, one byte from each
of four devices [2] ch. 2 p. 69.

## 5. Boot sequence summary

The family doc carries the boot contract (mdu.md §2.5, §2.6, §5.6); the IIci-specific sequence,
with the machine's own branch points:

1. **Power-on.** The trickle-charged capacitor switches the supply on within 2 s of the keyboard
   or rear-switch request [2] p. 244; a locked rear switch self-recovers after an AC outage [2]
   p. 245.
2. **Reset.** A Sony sound IC holds /RESET asserted until 0.25 s after the board voltages
   stabilize, putting the CPU, the general logic ICs, the internal devices and every expansion
   slot into their known initial states [2] p. 244.
3. **Overlay fetch.** The MDU has ROM mapped at physical $0000 0000 since reset (mdu.md §2.6), so
   the 68030's reset-vector fetch from $0000 0000 reads ROM [1] p. 13. Which alias of the
   duplicated ROM the vectors come from is not pinned by any printed source (§6.11).
4. **Map flip.** The first access to true ROM space ($4000 0000–$4FFF FFFF) makes the MDU impose
   the normal map — the only change being that $0000 0000–$3FFF FFFF stops selecting ROM and
   selects RAM [1] p. 13 (mdu.md §2.6). The release is access-triggered on this family; no VIA
   bit can force it (mdu.md §2.6, §7 item 6).
5. **RAM sizing.** The ROM sizes both banks by the wrap behaviour — "this address wrapping allows
   the ROM to determine how much memory is present in each bank" [1] p. 21 (mdu.md §3.6) — and
   "compiles a table describing the current memory configuration" from which the 68030's on-chip
   MMU presents contiguous logical memory [1] p. 15.
6. **Memory stitching.** The MMU maps bank B's physical $0400 0000 range to logical $0000 0000
   and places the remainder of bank A (above the reserved screen buffer) *above* bank B —
   "Even though the physical addresses of RAM bank B start at $0400 0000, the logical addresses
   start at $0000 0000, as required for software compatibility" [2] pp. 412–413, 221.
7. **Parity bring-up (parity models only).** Startup determines whether the PGC is installed,
   tests for 9-bit SIMMs, and enables parity only if both are found — first writing valid parity
   across all of RAM (§3.2.3, [2] p. 222).
8. **Video bring-up.** The RBV has latched the monitor sense at power-up; startup code reserves
   bank A's bottom for the maximum intended depth, maps the buffer to logical $FB00 0000, and
   switches the built-in video off entirely if no supported monitor is attached (§3.3,
   [rbv.md](rbv.md) §2.8, §4.5).
9. **Cache bring-up (cache card only).** The external cache is disabled after reset; the
   EnableExtCache trap (HWPriv selector 4) enables it and the card asserts CACHE from then on
   ([1] Table 4-2 p. 33, [rbv.md](rbv.md) §4.6, mdu.md §2.4).
10. **OS handoff.** From here the sequence is the Macintosh II-family startup of the Guide's
    step 6 — "The Reset handler carries out the startup procedure described in Inside
    Macintosh" [2] p. 244.

## 6. Open questions

1. **The project codename.** No document in this evidence set names the IIci's project; the
   Overview's pointer to an "Atlantic and Pacific Theory of Operation" [3] p. 8 supports
   "Atlantic" by position only (*inferred — unverified*, mdu.md §1.2). The codename "Aurora"
   sometimes attached to the machine appears nowhere in this corpus.
2. **The Gestalt and `SysEnvirons` identity values.** The machine-class constant 11 is known from
   the m68k kernel dispatch [6] (*observed*); no Apple document prints the IIci's Gestalt `mach`
   value, a BoxFlag, or a `SysEnvirons` machine type, and the printed sources give no ROM version
   number — the image's identity in this corpus is its checksum longword [5].
3. **The IIci's I/O sub-decode.** No Apple document prints it; the window layout used for this
   machine is inferred from the IIsi's printed figure, the shared device set and the PDM AMIC's
   later documented reuse of the same layout (mdu.md §7 item 1, [rbv.md](rbv.md) §6 item 1). A
   printed IIci decode or a logic-analyzer capture of a IIci board would close the family's
   biggest hole.
4. **The slot-interrupt register's bits 6–7.** The Guide's VIA2 table note names bit 6 `v2IRQ0`,
   "used for the interrupt from the internal video circuits", with bit 7 reserved [2] Table 4-9
   note p. 166; the Hardware Overview's IIci column instead labels bits 6–7 `RAM.SIZ0`/`RAM.SIZ1`
   [3] pp. 63–64. The conflict — and what the two bits actually do on a IIci, where the Guide
   also says the RAM-size bits "are not needed" because the MMU does the mapping [2] p. 166 — is
   unresolved ([rbv.md](rbv.md) §2.4, §6.4).
5. **/EXP.IRQ.** RBV IFR bit 2 is "Macintosh IIci only" in the Guide's table [2] Table 4-27 p. 186,
   but nothing in print says what it is wired to on this board ([rbv.md](rbv.md) §6, item 9).
6. **PAR.TEST~ and the chip-test register.** The RBV control byte's bit 7 pairs with a "signal
   used for testing the parity circuits" [2] p. 157, and the RBV's `rChpT` register is named by
   the kernel headers [6] — but no printed source describes the test sequence, and the *PGC User
   Manual* [3] p. 72 that would is not in this evidence set.
7. **The "8 MHz 8530 SCC" claim.** Only the Hardware Overview's comparison table rates the IIci's
   serial ports at 8 MHz [3] Table 1.4; no source states the SCC's PCLK on this machine, and the
   RBV's 3.672 MHz rate-setting clock [rbv.md](rbv.md) §1.6 does not by itself explain the figure
   (mdu.md §7 item 10).
8. **The MDU's own reset.** The IIsi's Egret resets the MDU first via Test (mdu.md §2.5); whether
   the IIci's single /RESET net gives the MDU any earlier or separate release is not printed
   (mdu.md §7 item 14).
9. **Slot access to power control.** The feature list states that NuBus cards can control power
   [1] p. 4, but no table in the corpus names the connector pin or the protocol — the Guide's
   closest statement is that /POWEROFF reaches the processor-direct slot on the SE/30 [2]
   p. 157, a machine with no NuBus slots to protect.
10. **The power-up map's extent below $4000 0000.** The Developer Note says ROM is selected for
    the whole of $0000 0000–$3FFF FFFF until the first true-ROM access [1] pp. 13–14; the Guide's
    startup sequence instead describes the overlay as decoding "either the range $0000 0000
    through $0FFF FFF or $4000 0000 through $4FFF FFFF" as ROM [2] p. 244 — a 16 MB figure that
    matches the GLUE machines' convention, not the IIci's printed map. Whether the Guide's
    sentence is an error or a real difference is unresolved (the related alias-ladder question is
    mdu.md §7 items 3–4).
11. **The overlay's first access and the reset vectors' alias.** Which instruction performs the
    map-flipping access, and therefore which alias the IIci's reset vectors and early Reset
    handler execute from, is ROM-image behaviour; no annotated IIci ROM disassembly exists in
    this evidence set [5] (mdu.md §7 item 5).
12. **The 512 × 384 mode.** The Hardware Overview's IIci column credits the built-in video with
    "512x384x1,2,4,8" [3] Table 1.4, but the Developer Note reserves the sense code (`010`) that
    the IIsi uses for that mode [1] Table 5-1 p. 45. Whether later IIci ROM revisions enabled the
    mode, or the Overview's table is simply wrong, is unresolved (mdu.md §7 item 12).
13. **The diagnostic pins on production boards.** Appendix B documents /ROMOE, /DSACK0~/1, /IPL0–2,
    /BR and CPUDIS on "no connection" pins [1] pp. 69–73, but no source confirms every production
    IIci board wires them, and nothing states whether any shipped ROM or tool ever reads them.
14. **Undecoded-space behaviour around the card windows.** What a processor access to the cache
    data/tag windows does when no card is installed — retry, /BERR, or an acknowledged read of
    nothing — is a case of the family's undecoded-address question (mdu.md §7 item 8), made more
    pointed on the IIci because the windows sit inside the documented I/O island.

## References

1. Apple Computer, Inc., *Macintosh IIci Developer Notes*, Developer Technical Publications, 1989.
   Chapter 1 Introduction pp. 3–10 (features and the MDU/RBV feature list pp. 3–5, system block
   diagram Figure 1-1 p. 6, hardware block diagram Figure 1-2 p. 7, disks/compatibility and
   II-family history p. 8, system comparisons Table 1-1 p. 9); chapter 2 Address Mapping
   pp. 13–17 (overlay p. 13, physical maps Figure 2-1 p. 14, MMU p. 15, 24-bit mapping Table 2-1
   p. 16); chapter 3 The RAM Interface pp. 21–28 (bank rules and wrap sizing p. 21, RAM
   configurations Figure 3-1 p. 22, use of RAM by video p. 23, DRAM specification Table 3-1 p. 24,
   refresh p. 25, SIMM pinouts Tables 3-2/3-3 pp. 26–27, PGC and parity pp. 27–28); chapter 4 The
   Cache Connector pp. 31–42 (SE/30 incompatibility and design rules p. 31, cache address space
   Table 4-1 and traps Table 4-2 p. 32–33, CACHE signal p. 33, signals Table 4-3 pp. 34–36,
   pinout Table 4-4 pp. 36–37, power Table 4-5 p. 39, mechanical pp. 39–42); chapter 5 The Video
   Interface pp. 45–54 (MON.ID Table 5-1 p. 45, RBV signal descriptions Table 5-2 p. 48, sync
   timing Figure 5-2 p. 49, monitor timing Figures 5-3/5-4 pp. 50–51, cables and connector pinouts
   pp. 52–54); chapter 6 The NuBus Interface p. 59; Appendix A RAM and ROM Timing Diagrams
   pp. 63–65 (MDU states and CPUCLK note p. 63); Appendix B Diagnostic Pinouts pp. 69–73 (CPUDIS
   p. 69, signal descriptions Table B-1 pp. 69–71, pinout Figure B-1 p. 72); Appendix C cache
   signal timing pp. 77–80 (parity/STERM/CACHE rule Figure C-1 p. 77, NuBus interaction
   Figure C-2 p. 78).
2. Apple Computer, Inc., *Guide to the Macintosh Family Hardware*, 2nd ed., Addison-Wesley, 1990.
   IIci placing and special-order parity model ch. 1 p. 29; IIci interrupts and the slot-style
   video interrupt ch. 2 p. 62; cache RAM two-clock hit p. 68; ROM arrangement ch. 2 pp. 68–69;
   interrupt levels, SCC and NMI via the RBV ch. 3 pp. 100–103; IIci address map ch. 3 p. 141;
   functions of VIA1 in the IIci ch. 4 p. 154; VIA2 functions in the IIci (RBV) ch. 4 p. 157;
   VIA1 Data register A Table 4-6 p. 164, identity codes Table 4-8 p. 165, VIA2 Data register A
   Table 4-9 with the v2IRQ0 note p. 166, VIA1 Data register B Table 4-14 pp. 171–172, RBV
   interrupt-flag Table 4-27 p. 186; v2BusLk p. 175; RAM access rate ch. 5 p. 195; video RAM and
   bank mapping p. 221; parity RAM p. 222; RAM cache card pp. 223–226; power up, startup and
   power down ch. 6 pp. 244–245; power supply connector Table 6-5 p. 250; AC input Table 6-10
   p. 256; output limits Tables 6-20/6-21/6-22 pp. 261–262; ADB circuit Figure 8-3 p. 294;
   FDHD interface Figure 9-13 p. 354; serial interface Figure 10-5 p. 369; SCSI interface
   Figure 11-8 p. 390; built-in video features p. 410, screen-buffer sizes Table 12-3 p. 411,
   pseudo-slot video p. 412, screen buffer pp. 412–413, video access percentages Table 12-4
   p. 413, CLUT DAC and connector pp. 418–420, sense lines Table 12-8 p. 420; ASC circuit
   Figure 13-5 p. 440; size and weight Table A-8 p. 484.
3. Apple Computer, Inc., *Macintosh Hardware Overview*, revision 2, 11 February 1991. Machine
   comparison Table 1.4 pp. 9–10 (the IIci column: intro date, cache slot, RBV modes, NUBUS30,
   SCC rating, ADB chip and VIA2 = RBV); MDU-family MMU note and the "Atlantic and Pacific"
   pointer p. 8; VIA/RBV bit assignments for the IIci pp. 63–64; RBV chip spec 343S1019-A and PGC
   User Manual reference lists p. 72.
4. Apple Computer, Inc., *Designing Cards and Drivers for the Macintosh Family*, 3rd ed.,
   Addison-Wesley, 1992. Macintosh IIci block diagram Figure 1-3 p. 23; NuChip 30 on the IIsi
   adaptor p. 170; chapter 23 Macintosh IIci Cache Memory Expansion pp. 517–528 (overview and
   warnings pp. 517–518, diagnostic signals pp. 519–521, signal descriptions Table 23-3
   pp. 521–523).
5. Macintosh IIci boot ROM, 512 KB image, header checksum longword `$368CADFE` (dumped image;
   header read for this page — no annotated disassembly of this image exists in the evidence
   set).
6. Linux kernel, m68k Macintosh machine support: `arch/m68k/include/asm/mac_via.h` (the RBV
   register-offset set and the `MACH_CLASSIIci` machine-class dispatch) and
   `arch/m68k/include/asm/macintosh.h` (the machine/Gestalt constants, IIci = 11) — the shipped
   multi-machine kernel headers whose dispatch routes the IIci and IIsi to one RBV offset set
   against the real-6522 path of the II/IIx/IIcx/SE/30.
