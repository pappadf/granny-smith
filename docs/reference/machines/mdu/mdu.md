# The MDU family — Macintosh IIci, IIsi

**Contents:**

1. [Overview & membership](#1-overview--membership) — the generation, its two machines and the near-miss
   designs, the custom-IC set and the division of labor, what this page carries
2. [Board architecture common to the family](#2-board-architecture-common-to-the-family) — the two-bank RAM
   structure, the MDU's full treatment (functions, timing, ROM control, bus-master and cache contracts,
   soft power and the ROM overlay)
3. [Memory map & address decode shared by the family](#3-memory-map--address-decode-shared-by-the-family)
   — the normal and power-up physical maps, the I/O island, NuBus space and its CPU-side exclusion,
   24-bit mode, bank windows and wrap sizing, expansion windows, the error contract
4. [Device roster](#4-device-roster) — every chip on the board and where its page lives
5. [Interrupt, bus, and clock architecture](#5-interrupt-bus-and-clock-architecture) — interrupt levels,
   slot and device interrupt paths, arbitration and bus mastership, bus error, the clock tree,
   reset distribution
6. [Per-machine index](#6-per-machine-index) — the IIci, the IIsi, and the designs that never shipped
7. [Open questions](#7-open-questions)
- [References](#references)

---

## 1. Overview & membership

### 1.1 What the family is

The **MDU family** is Apple's first fully integrated 68030 desktop generation: the **Macintosh IIci**
(September 1989) and the **Macintosh IIsi** (October 1990), two main logic boards built around one
custom chip pair. The pair is named in both machines' developer notes in the same breath: the IIci is
"the first in a new series of Macintosh computers compatible with the Macintosh II family … The new
architecture is based upon the Memory Decode Unit (MDU) and RAM-based video chips (RBV)"
[1] §"Features" p. 3, and the IIsi "shares many, but not all of the features of the more powerful
Macintosh IIci computer. Like that of the Macintosh IIci, the architecture of the Macintosh IIsi is
based on the Memory Decode Unit (MDU) and RAM-Based Video (RBV) chips" [2] ch. 1. The feature both
notes lead with is the same one: "a new chip set provid[ing] memory decoding and low-cost video by
utilizing existing on-board DRAM for the frame buffer" [1] p. 3, [2] ch. 1 — the frame buffer is not
separate video RAM but the bottom of ordinary system DRAM, bank A, from physical address $0000 0000
[1] p. 23, [2] ch. 3, [3] p. 221.

The family is named after the **MDU**, Apple's Memory Decode Unit, because that part is the
generation's defining piece: it is the address decoder, memory controller, DRAM-refresh engine,
device-select generator, bus-error monitor and clock source of both boards, taking over the role the
GLUE ASIC plays in the Macintosh II/IIx/IIcx/SE/30 generation and adding the 68030-specific
behaviour the older part never had — burst transfers terminated with /STERM, a two-bank RAM interface
that serves the CPU and the video fetch at once, and an access-triggered ROM-at-zero overlay
(§2.2, §2.6). Around it the RBV concentrates the interrupts, replaces the second VIA and scans the
frame buffer out of bank A ([rbv.md](rbv.md) §1.2), and the NuChip30 adapts the NuBus controller to
the 68030 bus [1] p. 8.

Apple's own summary of the generation is the IIci's feature list: 25 MHz 68030 with on-chip MMU and
burst reads into the internal caches, on-board video for the 12" B&W, 13" RGB and 15" B&W Portrait
monitors, optional DRAM parity, an optional external cache card, three NuBus slots, 80 ns fast-page
DRAM in two banks of four SIMM sockets each, a ROM SIMM, and soft power control [1] pp. 3–4. The
IIsi re-casts the same architecture as a low-cost 20 MHz machine: 1 MB of RAM soldered down, a single
120-pin expansion connector whose user-installed adaptor selects a processor-direct slot or one NuBus
slot, the FPU moved off the board onto those adaptors, an integrated serial/SCSI "Combo" chip, and an
ADB microcontroller that also owns the real-time clock, parameter RAM, soft power, reset and NMI
[2] ch. 1.

### 1.2 Membership

| Machine | Introduced | CPU, clock | FPU | RAM | Expansion | Companion controller | Page |
|---|---|---|---|---:|---|---|---|
| Macintosh IIci | September 1989 [4] Table 1.4 | MC68030, 25 MHz | 25 MHz 68882, standard [1] Table 1-1 p. 9 | 1–128 MB, 80 ns, two SIMM banks [1] p. 21 | three NuBus slots ($C–$E) + cache connector | discrete RTC + ADB transceiver behind VIA1 [1] Figure 1-2 p. 7, [4] Table 1.4 | [iici.md](iici.md) / §6.1 |
| Macintosh IIsi | October 1990 [4] Table 1.4 | MC68030, 20 MHz | none standard; 20 MHz 68882 on either expansion adaptor [2] ch. 1 | 1–65 MB, 100 ns, soldered bank A + four SIMM sockets [2] ch. 3 | one 120-pin connector; PDS or NuBus by adaptor kit [2] ch. 6 | Egret 68HC05-class microcontroller (ADB, RTC, PRAM, power, reset, NMI) [2] ch. 5 | [iisi.md](iisi.md) / §6.2 |

Two designs that never shipped in this form belong to the family's history. Apple's internal
machine-comparison table of February 1991 lists a **"Buccaneer"** project — a 40 MHz 68030 with the
same MDU/RBV/NuChip30/Egret chip set and RBV as its VIA2 — as then unreleased [4] Table 1.5, and the
same table groups the IIci with **"Tim"**, the fixed-display ASIC family of the Classic II class, in
its interrupt-priority figure [4] Figure 2-1 p. 16 (see [video-overview.md](../../hardware/video-overview.md) §3.4
for Tim's video role). Neither Buccaneer's shipped fate nor its relation to any production board is
established from the printed record (§7). The wider RBV register lineage — the LC's V8 part and the
Power Macintosh AMIC's pseudo-VIA2 — is RBV history, not MDU-family membership, and is covered on the
RBV page ([rbv.md](rbv.md) §1.3, §1.5, §5).

Apple's project names surface in the printed record only obliquely: the Hardware Overview's tables
head the IIsi column "Mac IIsl" [4] Table 1.4, its I/O-map figure note in the IIsi Developer Note
carries the board name "Ray Ban" ([iisi.md](iisi.md) §5), and the Overview sends developers to an
Apple-internal "Atlantic and Pacific Theory of Operation" for "how Mac IIci uses the MMU to
implement its memory mapping" [4] p. 8 — Atlantic and Pacific being, by position, the IIci and IIsi
projects' names (*inferred*; no Apple document in this evidence set states the mapping outright).

### 1.3 The custom-IC set and the division of labor

The MDU family's component split, from the IIci hardware block diagram [1] Figure 1-2 p. 7 and the
Guide's component descriptions [3] pp. 115–117:

| Function | Owner | Where documented |
|---|---|---|
| Address decode, device selects, DSACK-width acknowledgement, DRAM control and refresh, burst support, ROM overlay, bus-error monitoring, CPU and E clocks, SCSI pseudo-DMA handshaking | the **MDU** | this page §2.2–§2.4 |
| Frame-buffer addressing inside bank A (the MDU generates the addresses at the RBV's request) | the MDU | §2.2, [3] pp. 115, 117 |
| VIA2 emulation, interrupt concentration, slot-interrupt flags, NuBus status bits, bus lock, cache enable/flush strobes, power-off, video fetch engine, monitor sense, sync generation | the **RBV** | [rbv.md](rbv.md) §1.2, §2, §3 |
| NuBus arbitration and the NuBus side of the slots | **NuChip30** (on the IIci board; on the IIsi it lives on the NuBus adaptor card) | [1] p. 8, p. 59, [2] ch. 6, [nubus.md](../../hardware/nubus/nubus.md) |
| CLUT + video DAC | the **VDAC** (a Brooktree Bt478-class part, "VDAC 478 D/A & Color Lookup" in the IIci diagram) | [1] Figure 1-2 p. 7, [3] p. 417, [rbv.md](rbv.md) §1.4 |
| VIA1 — ADB transceiver interface, RTC interface (IIci), 60.15 Hz VBL, one-second tick, machine-ID straps | a real 6523-class VIA | [2] ch. 5, [4] pp. 63–64, [via.md](../../hardware/via.md) |
| DRAM parity generation and checking (IIci option only) | the **PGC**, Parity Generator Chip | [1] pp. 27–28, [3] p. 222 |
| ADB, RTC, PRAM, soft power, power-on reset, keyboard reset and NMI (IIsi only) | the **Egret** microcontroller | [2] ch. 5, [egret.md](egret.md) |
| Serial (SCC) and SCSI | IIci: discrete 8530 SCC + 53C80 SCSI; IIsi: the **Combo** chip integrating both | [1] Figure 1-2 p. 7, [2] ch. 5, §4 |
| Sound output | the ASC plus two Sony sound chips | [asc.md](../../hardware/asc.md) §6, [2] ch. 5 |
| Floppy | SWIM | [iwm-floppy.md](../../hardware/iwm-floppy.md), [2] ch. 5 |

The load-bearing consequence of this split is what the MDU does **not** own. It is not in the
interrupt path beyond generating the clocks that pace it — the RBV concentrates interrupts
([rbv.md](rbv.md) §3.1). It is not in the video signal path beyond fetching the data — "The RBV knows
nothing about screen mapping or video addresses. Likewise, the MDU knows nothing about video. Each
simply follows a protocol for passing data" [1] p. 47, [2] ch. 4. And it holds no slot-interrupt or
VIA state — VIA1 and the RBV's VIA2 emulation divide that between them (§5.1). A re-implementer who
puts any of those jobs in the MDU has the architecture wrong.

### 1.4 What this page carries

This page is the **family doc** of the three-level machine set: it holds what both boards share.
Three exclusions keep it honest to the "say it once" rule. The RBV's registers, behaviour and
programming model live on its own device page and are cited here by section, never restated
([rbv.md](rbv.md) §2–§4). Machine-unique wiring — the IIci's cache connector and PGC, the IIsi's
Egret, Combo chip and expansion adaptors — lives on the machine pages and is indexed in §6. And the
**MDU itself has no device page of its own: its full treatment lives here** (§2.2–§2.6), because the
memory controller is the family rather than a part one board borrows.

## 2. Board architecture common to the family

### 2.1 The two-bank RAM structure

Both boards divide RAM into two banks at fixed physical windows: bank A at $0000 0000–$03FF FFFF and
bank B at $0400 0000–$07FF FFFF, each a 64 MB window regardless of what is installed [1] p. 21,
[2] ch. 3. What differs is the population: the IIci gives **both** banks four SIMM sockets each —
a bank holds nothing or four equal SIMMs of 256 KB, 1 MB, 4 MB or 16 MB, so a populated bank is
exactly 1, 4, 16 or 64 MB [1] p. 21 — while the IIsi solders bank A down as a fixed 1 MB built from
eight 256K×4 fast-page-mode DRAMs and puts only the four expansion SIMM sockets in bank B
[2] ch. 3. Bank A must be populated for on-board video to work [1] p. 21, and the IIci's
configuration note adds the performance rule: "For best performance with on-board video, put the
smaller SIMMs in bank A" [1] Figure 3-1 note p. 22.

The two banks are not merely two address ranges — they are two memory systems with a controllable
data path between them. The RBV and bank A share a RAM data bus that connects to the CPU data bus
through F245 bus buffers; during a video fetch the MDU disconnects the buffer, so the RBV reads bank A
while the CPU keeps full access to bank B, ROM and the I/O devices [1] pp. 23–24, Figure 3-2 p. 24,
[2] ch. 3. "Each bank of RAM is accessed independently by the MDU, so it can decode addresses for the
CPU and the RBV at the same time without interference" [2] ch. 3 — the Guide's phrasing of the same
fact: "The MDU provides separate memory addresses and strobes for the two banks of RAM (bank A and
bank B), making it possible for the main processor and the video circuits to use RAM at the same
time" [3] p. 115. Only CPU accesses to bank A ever wait for video; the contention percentages and
the RBV's precedence inside bank A are RBV behaviour ([rbv.md](rbv.md) §3.7).

The row/column multiplexing is drawn at the MDU: the block diagram shows the MDU driving the banks
with separate row/column address groups RAA0–11 and RAB0–11 [1] Figure 1-2 p. 7 (*observed* in the
diagram; the printed text never names the strobes). The SIMM standard is shared: 30-pin SIMMs with
bypass capacitors, 8 data bits wide, the IIci optionally 9 bits wide for parity [1] pp. 24–27,
[2] ch. 3.

### 2.2 The MDU: what the chip does

The Guide is the one Apple document that enumerates the part, and its enumeration is worth taking in
full [3] pp. 115–116. **Functions specific to the MDU generation:**

- It "provides separate memory addresses and strobes for the two banks of RAM (bank A and bank B)",
  enabling simultaneous CPU and video use of RAM (§2.1).
- It "provides address decoding and device-select signals for new devices: the RBV, the video CLUT
  DAC, and the NuChip30 (NuBus controller)".
- It provides the addressing for the video buffer in bank A — "but it has nothing further to do with
  the generation of the video signals" [3] p. 115.

**Functions inherited from the GLUE** [3] p. 115:

- It decodes addresses to determine which device or auxiliary processor the main processor is
  requesting, and asserts the device-select signal to that device.
- It "sends acknowledge signals to the main processor that indicate that a device is present and
  that specify the width of that device's data bus" — the DSACK/STERM contract of §2.3.
- It generates the signals that refresh dynamic RAM.
- It generates the CPU clock — 25 MHz in the IIci; the IIsi runs the same design at 20 MHz
  ([1] p. 9; [2] ch. 1; §5.5).
- It generates the 783.36 kHz **E clock** used to synchronize communication between the VIA and the
  main processor.
- It "monitors data transfers and generates the Bus Error signal to halt the main processor if a
  transfer fails to complete successfully" (§5.4).
- It "handles hardware handshaking with the SCSI controller", holding each pseudo-DMA byte until the
  controller's DRQ line goes high — the II-family mechanism that makes blind SCSI transfers safe
  [3] p. 393.

Internally the MDU is a clocked state machine, not a asynchronous decode puddle: the IIci's timing
appendix draws its RAM cycles in "MC68030 states" (the s-numbers) and **MDU states** (the large
numbers), "referenced to the CPUCLK", with "both the rising and falling edges of the CPUCLK … used
to change states" [1] Appendix A p. 63.

What no source in this evidence set gives the part is a register file. Neither developer note
describes a single MDU register, the Guide attributes none, and Apple's own reference list names an
*MDU User Manual* (Michael Dhuey) [4] p. 72 that is not present in this corpus. Every software-visible
behaviour of the family therefore bottoms out in one of three places: the devices the MDU selects, the
MDU's bus contracts (§2.3–§2.4), or its map switching (§2.6). Whether the part has any
software-addressable test or configuration registers at all is open (§7); a re-implementation
modelled purely on decode-and-acknowledge behaviour matches every documented contract.

### 2.3 MDU timing: RAM, ROM and refresh

The timing contracts, identical in both notes apart from the DRAM speed grades [1] pp. 21, 24–25 and
Appendix A pp. 63–65, [2] ch. 3, [iisi.md](iisi.md) §6.2:

| Cycle | Duration (CPU clocks) | Notes |
|---|---|---|
| Burst read from RAM | 5-clock initial access, then three 2-clock accesses | fills a 68030 cache line (4 longwords) [1] p. 21, Figure A-1 p. 63 |
| Random read from RAM | 5-clock minimum; 6 clocks when delayed by a preceding write | [1] Figure A-2 p. 64 |
| Random write to RAM | 4-clock minimum; 5 clocks when delayed by a preceding write cycle | [1] Figure A-3 p. 64 |
| Refresh cycle | 6 clocks, every 15.6 µs | "can overlap previous RAM cycle by 1 clock; next RAM cycle may be held off for 5 clocks" [1] Figure A-4 p. 65 |
| ROM read | 5 clocks | [1] Figure A-5 p. 65, [2] ch. 3 |

RAM termination is **synchronous**: the MDU answers with /STERM, not /DSACK, for RAM cycles
[2] ch. 3, 6 — the contract that makes 68030 burst fills possible and the one an IIsi PDS bus master
must honour (§2.4). Refresh is /CAS-before-/RAS, initiated at the same instant in both banks but
continuing independently per bank, so a refresh held off behind a CPU or video access in one bank does
not hold off the other bank's; "refresh does not affect the processor at all if the processor is
addressing anything except RAM" [1] p. 25, [2] ch. 3.

The DRAM grades the timing assumes:

| Parameter | IIci [1] Table 3-1 p. 24 | IIsi [2] ch. 3, Table 3-1 |
|---|---|---|
| RAS access time | 80 ns | 100 ns |
| CAS access time | 20 ns | 25 ns |
| Access type | fast page mode | fast page mode |
| Refresh type | /CAS before /RAS | /CAS before /RAS |
| Refresh period | "15.6 ms" | 15.6 µs |

The IIci note's "15.6 ms" is a misprint, not a fourth design: the same document's timing appendix
draws the refresh cycle as "6 clock cycle every 15.6µS" [1] Figure A-4 p. 65, the IIsi note's
identical table says 15.6 µs, and a 15.6 ms refresh period is not a DRAM specification any 1989 part
could meet. The µs figure is the one to implement.

ROM is 512 KB on both boards, but it is deliberately not fast: five clocks per access, and **the MDU
does not support burst reads in the ROM address space** [2] ch. 3 — the 68030's cache-line fills from
ROM degrade to single beats. The physical ROM arrangement is machine-specific (the IIci's ROM SIMM
and sockets, the IIsi's early 512 KB ROM SIMM versus the later soldered 4-Mbit 256K×16 device in a
44-pin quad flat pack, on a board with capacity for 1 MB) [1] Table 1-1 p. 9, [2] ch. 3, and is
covered with the machine pages (§6); what is shared is the five-clock access, the no-burst rule, the
alias mirroring of §3.1 and the overlay of §2.6.

### 2.4 Bus masters, the cache and the MDU's external contracts

Three expansion contracts of the family bottom out in the MDU.

**The IIsi PDS bus-master contract.** A processor-direct card mastering the RAM bus on the IIsi sees
"a memory cycle … substantially different from that of the Macintosh SE/30 computer. It has been
changed to support burst transfers using the /STERM signal generated by the MDU rather than the
/DSACK signal generated by the general logic unit chip. If bus master cards look only for /DSACK,
they will not work" [2] ch. 6, [5] ch. 15. This is the MDU's synchronous termination exported to the
connector, and it is the single largest incompatibility between an SE/30 PDS card and the IIsi
(electrically the slots are otherwise identical [2] ch. 6). The connector also carries an /NUBUS
output ("NuBus space access") marking cycles whose address decodes to NuBus slot space, and the
/TM0A and /TM1A inputs the RBV records as its NuBus transfer-mode status bits ([rbv.md](rbv.md)
§2.3), plus /BUSLOCK and three /IRQ inputs [2] Table 6-1.

**The IIci cache-card contract.** The IIci's 120-pin cache connector (a different pinout from the
SE/30 PDS, and mechanically incompatible with it [1] p. 31) reaches into the MDU through one signal:
**CACHE**, active high, "disables the memory controller (MDU), so that it will not start a memory
cycle and will allow the cache to supply the data instead" [1] p. 33, [3] p. 224. CACHE must transition
at /AS or earlier; asserted after the MDU has started a cycle, that cycle completes unaffected; and
it has no effect on the MDU's cycles for I/O devices. On a miss the card forces a processor retry with
/BERR and /HALT and deasserts CACHE so the MDU performs the access on the retry [3] p. 224. The
enable and flush strobes the card obeys are the RBV's (CENABLE~/FCFLUSH~, [rbv.md](rbv.md) §3.8), and
the card's data and tag memories are reached through reserved windows at $5200 0000–$527F FFFF and
$5280 0000–$52FF FFFF that the card itself decodes — "no select signal is provided on the connector"
— and that the 24-bit map cannot reach [1] pp. 31–32, Table 4-1 p. 32. Two design rules Apple states
at family level: "Do not cache accesses made by bus masters other than the 68030, since they may not
know how to retry", and the BGACK~ line "is not driven high quickly enough by the motherboard" — a
cache card must pull BGACK~ up with 2.2 kΩ and double-rank synchronize it before use [1] p. 34.

**Emulator and diagnostic support.** The IIci cache connector's "no connection" pins are populated
on real boards with signals "provided only for debugging and emulator support": /ROMOE, /DSACK0~/1,
/IPL0~–2, /BR and **CPUDIS**, which "is used to disable the MC68030 on the motherboard and render
its outputs high-impedance. An emulator in the cache connector may assert CPUDIS and, after waiting
for the end of the current bus cycle, may drive all signals" [1] Appendix B p. 69, [5] ch. 23. Apple
warns these may not be supported in future implementations of the connector; they are the family's
documented hardware-level debugging port, and they are the ancestor of the IIci-class machines'
emulator-hosting convention.

### 2.5 Soft power, reset and the power-up sequence

The family inherits the II-family soft-power design, in which the supply is switched by a trickle
circuit rather than a hard mains switch. On the IIci the switching capacitor "is kept charged by a
trickle current from the power supply" (not by batteries, as on the Macintosh II/IIx), the rear
mechanical switch can be locked on so the machine recovers by itself after an AC outage, and software
shutdown asserts a power-off control bit — on the IIci the RBV's, on the IIsi the Egret's — that
drops the supply 2 ms later [3] pp. 244–245, [1] p. 4, [2] ch. 5. The keyboard power-on path runs
through the ADB connector into the power-control circuits without involving the ADB transceiver
[3] p. 77. The control-bit details are device behaviour (power-off: [rbv.md](rbv.md) §3.9; Egret's
PFW: [egret.md](egret.md), [iisi.md](iisi.md) §13.4).

Reset distribution differs between the machines in one load-bearing way. On the IIci it is the
II-family flow: a Sony sound IC monitors the board voltages and holds /RESET asserted until 0.25 s
after they stabilize; /RESET "causes the CPU, the general logic ICs, and all of the internal devices
to come to a known initial state" and reaches every expansion slot; then the CPU fetches its reset
vectors from $0000 0000 [3] p. 244. On the IIsi the Egret owns the same job and splits it: when it
turns the supply on it asserts both **Reset** and **Test**, where "the Test signal, which has a
shorter time constant than Reset, is used to reset the MDU. It allows the gate array to initialize
the RAM controller and the I/O decode circuits **before** the processor attempts to execute any
cycles. The Reset signal … goes to the 68030 and other I/O devices, [and] has a longer time constant
than Test and allows the processor to stabilize before executing any cycles" [2] ch. 5. The MDU is
therefore the first device out of reset on the IIsi, by design — the decode and RAM machinery must be
live before the first CPU cycle.

### 2.6 The ROM overlay and its release

Both boards power up with the entire low gigabyte addressed to ROM. "On power up, ROM is mapped by
the MDU to physical location $0000 0000. This enables the starting address, retrieved by the 68030
on reset, to be stored in ROM. After the first access to the true ROM address space ($4000 0000
through $4FFF FFFF), the normal memory map is imposed by the MDU. The only change from one map to
the other is that in the power-up map ROM is selected for addresses $0000 0000–$3FFF FFFF, whereas
the normal map selects RAM for that address space" [1] p. 13, [2] ch. 2.

The trigger is an **access**, not an instruction: any reference into $4000 0000–$4FFF FFFF flips the
MDU to the normal map. This is the family's cleanest divergence from the GLUE machines it descends
from, where the overlay is a VIA1 output pin — "when VIA1 is reset, it pulls the Overlay signal
high … one of the first instructions in the Reset handler causes the Overlay signal to go low" [3]
p. 244 — and the Guide marks the difference explicitly: "(In the Macintosh IIci, the MDU performs
ROM overlay automatically)" [3] p. 153. On the MDU machines VIA1 port A bit 4 is not an overlay
output but part of the machine-ID strap field (the GLUE machines' PA4 = OVERLAY becomes CPU.ID2 on
the IIci) [4] pp. 63–64; no VIA bit can force the overlay back on. What performs the release in
practice is the ROM's own early code touching true ROM space; the exact first access is ROM-image
behaviour, not a family property (§7).

## 3. Memory map & address decode shared by the family

### 3.1 The normal physical map

The normal map, imposed by the MDU after the overlay release ([1] Figure 2-1 p. 14, [2] Figure 2-1;
the same map in table form for the IIsi at [5] Table 15-10 p. 344 and [iisi.md](iisi.md) §3.2):

| Physical range | Meaning |
|---|---|
| $0000 0000–$03FF FFFF | **RAM bank A** (64 MB window); video screen buffer at the bottom when on-board video is active |
| $0400 0000–$07FF FFFF | **RAM bank B** (64 MB window) |
| $0800 0000–$3FFF FFFF | reserved RAM space |
| $4000 0000–$4FFF FFFF | **ROM**: the 512 KB image, then images of it at growing alias sizes; reserved above the top alias |
| $5000 0000–$52FF FFFF | I/O devices (§3.2) |
| $5300 0000–$5FFF FFFF | expansion I/O space |
| $6000 0000–$EFFF FFFF | NuBus super slot space |
| $F000 0000–$F0FF FFFF | reserved, no device assigned (§3.4) |
| $F100 0000–$F8FF FFFF | NuBus standard slot space, presently unused slots [3] Table 3-10 pp. 139–140 |
| $F900 0000–$FFFF FFFF | NuBus standard slot space, slots $9–$E: the IIci's three connectors are slots $C–$E [1] p. 59, the IIsi's single slot is $9 [2] ch. 6 |

Within the ROM region the 512 KB image is mirrored at increasing alias sizes — the printed figures
mark boundaries at $4008 0000, $4010 0000, $4020 0000 and above ([1] Figure 2-1 p. 14, [2]
Figure 2-1; the ladder as read from the IIsi figure is tabulated at [iisi.md](iisi.md) §3.2), and the
IIci block diagram sizes the ROM subsystem ".5–32 MB" [1] Figure 1-2 p. 7. The region above the top
alias is reserved ROM space.

### 3.2 The power-up map

Before the first access to $4000 0000–$4FFF FFFF (§2.6), the MDU selects ROM for the whole of
$0000 0000–$3FFF FFFF — a full gigabyte of duplicated ROM images, the reset vectors at the bottom
included [1] pp. 13–14, [2] ch. 2. Nothing else in the map changes between the two states; the I/O
island, NuBus space and ROM's true window decode identically in both.

### 3.3 The I/O island

The family's I/O lives in a 48 MB island at $5000 0000–$52FF FFFF, with expansion I/O space above it
([1] Figure 2-1 p. 14, [2] Figure 2-1). The sub-decode is printed only for the IIsi — [2] ch. 5
Figure 5-1, tabulated at [5] Table 15-10 p. 344 and [iisi.md](iisi.md) §5.2 — and is:

| Physical range | Device | Address lines wired | Data lane |
|---|---|---|---|
| $5000 0000–$5000 1FFF | VIA1 | A9–A12 | D24–D31 |
| $5000 2000–$5000 3FFF | reserved (VIA2's II-family window; no second VIA exists here) | — | — |
| $5000 4000–$5000 5FFF | SCC | A9–A12 | D24–D31 |
| $5000 6000–$5000 7FFF | SCSI, pseudo-DMA mode with DRQ | — | D24–D31 |
| $5000 8000–$5000 FFFF | reserved | — | — |
| $5001 0000–$5001 1FFF | SCSI, normal mode | — | — |
| $5001 2000–$5001 3FFF | SCSI, pseudo-DMA mode without DRQ | — | — |
| $5001 4000–$5001 5FFF | ASC (sound) | A4–A6 | D24–D31 |
| $5001 6000–$5001 7FFF | SWIM (floppy) | A9–A12 | D24–D31 |
| $5001 8000–$5002 3FFF | reserved | — | — |
| $5002 4000–$5002 5FFF | VDAC (CLUT + video DAC) | — | D24–D31 |
| $5002 6000–$5002 7FFF | RBV | A0, A1, A4 | 8-bit bus of its own |
| $5002 8000–$5FFF FFFF | reserved / expansion I/O (see below) | — | — |

Address-line and data-lane column for the VIA1/SCC/SWIM/SCSI group from the IIci hardware block
diagram, which draws each of them with "(A9–12)(D24–31)" and the ASC with "(A4–6)(D24–31)" [1]
Figure 1-2 p. 7; the RBV's A0/A1/A4 decode and separate 8-bit register bus are established on the RBV
page ([rbv.md](rbv.md) §2.1). The 8 KB sub-windows are the II-family convention (register-select
RS3–RS0 on A9–A12), which is why VIA-compatibility offsets like the RBV's $1A03/$1C13 aliases work at
all ([rbv.md](rbv.md) §2.5–§2.6).

The IIsi's figure adds two further facts the table above flattens: the island's low 256 KB
($5000 0000–$5003 FFFF) repeats in image through the reserved space above it, and a factory-test
region sits at $5800 0000–$5900 0000 ([iisi.md](iisi.md) §5.2). Whether the IIci's island mirrors
with the same period is not printed anywhere; the IIci sub-decode as a whole is **inferred** from the
IIsi's printed one plus the shared device set and the PDM platform's later documented reuse of the
same window layout (*inferred — unverified*, the open question at [rbv.md](rbv.md) §6, item 1). Accesses
the MDU does decode are acknowledged with the device's bus width ([3] p. 115); what an access to a
reserved sub-window returns is not printed (§7).

### 3.4 NuBus space, and the window the CPU must not touch

NuBus decode is geographic, as on the rest of the II family, but the MDU generation's slot
assignments are its own. The IIci "NuBus interface … remains the same as the Macintosh IIcx, except
that the slots are numbered 4 through 6 and mapped to geographic addresses $C through $E. On the
Macintosh IIcx, they were numbered 1 through 3 and mapped to geographic addresses $9 through $B.
This should not matter to the cards" [1] p. 59. The strap coding that produces it [1] Table 6-1
p. 59:

| Slot number | Geographic address | GA3~ | GA2~ | GA1~ | GA0~ |
|---|---|---|---|---|---|
| 4 | $C | GND | GND | open | open |
| 5 | $D | GND | GND | open | GND |
| 6 | $E | GND | GND | GND | open |

The IIsi's single slot (via the NuBus adaptor) is geographic $9, and "the different address mappings
are transparent to the cards" [2] ch. 6. The built-in video occupies no physical slot — it is bank A
at physical $0000 0000 — but the operating system maps it into slot space like a video card: slot $B
on the IIci, super-slot $E on the IIsi ([rbv.md](rbv.md) §4.5, [iisi.md](iisi.md) §4.2). The IIsi's
PDS cards take the pseudo-slot addresses $F900 0000–$FBFF FFFF, geographic $9–$B [2] ch. 6.

One range is reserved against the processor by design: **$F000 0000–$F0FF FFFF**. "An attempt to read
or write to an address in the range $F000 0000 through $F0FF FFFF from the main processor results in
a bus error; this address range is used by NuBus cards to address the I/O devices and ROM on the
main logic board" [3] p. 139. The NuBus-side translations the range exists for [3] Table 3-10
pp. 139–140:

| NuBus address | Main-logic-board address | Used to access |
|---|---|---|
| $0000 0000–$3FFF FFFF | same | RAM |
| $F000 0000–$F07F FFFF | $5000 0000–$507F FFFF | main logic board I/O devices |
| $F080 0000–$F0FF FFFF | $4080 0000–$40FF FFFF | ROM |
| others | same address | NuBus slot space / super slot space |

So a NuBus card on the IIci or IIsi can reach the family's I/O island and (part of) ROM across the
NuChip30, but the processor cannot follow it into the $F0xx window. This is the MDU generation's
one deliberately asymmetric decode, and it is enforced by the bus-error path (§5.4), not by
returning data.

### 3.5 24-bit compatibility mapping is not an MDU function

The 24-bit mode both machines support for pre-Macintosh-II software is produced entirely by the
68030's on-chip MMU: "The memory maps are set up by the 68030's on-chip MMU" [1] p. 15, [2] ch. 2.
The MDU sees only the resulting 32-bit physical addresses. The 1 MB-per-slot alias tables differ
between the machines — the IIci's marks NuBus $9–$B "not on IIci" and names slots $C–$E, the IIsi's
names $9–$B the PDS pseudo-slots, $C–$D unused and $E the on-board video [1] Table 2-1 p. 16, [2]
Table 2-1 — and are tabulated on the machine pages ([iisi.md](iisi.md) §4.2). For re-implementation
the family-level contract is: nothing in the MDU depends on, or reacts to, 24-bit mode; a model may
implement the whole compatibility map as MMU translation above an MDU that never knows it exists.

### 3.6 Bank windows, wrap and RAM sizing

Because each bank sits in a fixed 64 MB window and a populated bank holds at most 64 MB, any smaller
bank wraps: "unless 16-Mbit DRAMs are used in a bank of memory, some part of the 64 MB address space
will be unused. Such space will wrap, containing multiple images of the existing RAM in that bank's
address space" — 1 MB in bank A appears 64 times through $03FF FFFF, for instance [1] p. 21, [2]
ch. 3. The wrap is not a footnote; it is the sizing mechanism: "this address wrapping allows the ROM
to determine how much memory is present in each bank" [1] p. 21, [2] ch. 3. Software then "compiles a
table describing the current memory configuration" and programs the MMU to present contiguous
logical memory from "the potentially noncontiguous physical segments in Banks A and B" [1] p. 15,
[2] ch. 2 — the MDU's physical discontinuity is the reason the family's logical RAM needs stitching
at all ([4] p. 8). The IIci note's worked example of the stitch: "the Macintosh IIci maps the
portion of bank A used for main memory immediately above the memory in bank B", so logical $0000 0000
is bank B's physical $0400 0000 [3] p. 221.

### 3.7 Expansion windows

Two expansion address windows are family property. The IIsi's PDS pseudo-slot window
$F900 0000–$FBFF FFFF (§3.4) is where a processor-direct card's declaration ROM and registers are
expected, by analogy with a NuBus card [2] ch. 6. The IIci's cache-card windows — data at
$5200 0000–$527F FFFF, tags at $5280 0000–$52FF FFFF — are inside the I/O island's expansion space
but decoded by the card itself; they are unreachable in 24-bit mode [1] Table 4-1 p. 32 (§2.4).

### 3.8 Undecoded space and the error contract

The MDU "monitors data transfers and generates the Bus Error signal to halt the main processor if a
transfer fails to complete successfully" [3] p. 115. Three family behaviours ride on that sentence:

1. **The $F0xx window** faults processor accesses on purpose (§3.4) [3] p. 139.
2. **SCSI pseudo-DMA timeouts**: "if the read or write operation over the SCSI bus is not completed
   within certain time (different for different machines), the general logic IC asserts a bus error
   (/BERR) to the CPU" — on this family the general-logic IC is the MDU [3] p. 393. The duration is
   not printed (§7).
3. **NuBus transaction errors** are reported to software through the RBV: the NuChip30 asserts the
   bus error to the CPU and drives /TM0A//TM1A with the error type, which the RBV records in its
   control byte for software to read [3] p. 156 ([rbv.md](rbv.md) §2.3, §3.3).

What an access to an otherwise undecoded physical address does — timeout and /BERR, immediate /BERR,
or an acknowledge with garbage data — is not stated for the family as a whole; only the $F0xx and
SCSI cases are documented explicitly (§7).

## 4. Device roster

### 4.1 Devices both boards carry

| Device | Role on both boards | Page |
|---|---|---|
| Motorola MC68030 | 25 MHz (IIci) / 20 MHz (IIsi); 256-byte instruction and data caches; on-chip MMU; burst reads into the caches [1] p. 4, [2] ch. 1 | — |
| MDU | address decode, memory control, refresh, clocks, bus error (§2.2) | this page §2 |
| RBV | VIA2 emulation, interrupt concentration, video control ([rbv.md](rbv.md) §1.2) | [rbv.md](rbv.md) |
| NuChip30 | NuBus controller, adapted "to work efficiently with the 68030 bus" [1] p. 8; on the IIci on the board, on the IIsi on the NuBus adaptor card [2] ch. 6 | [nubus.md](../../hardware/nubus/nubus.md) |
| VIA1 (6523-class) | ADB transceiver interface and RTC interface on the IIci; 60.15 Hz VBL on CA1, 1-second tick on CA2, machine-ID straps on port A [4] pp. 63–64 | [via.md](../../hardware/via.md) |
| VDAC | CLUT + triple video DAC for the built-in video ([rbv.md](rbv.md) §1.4) | [rbv.md](rbv.md) §1.4, [3] p. 417 |
| SWIM | floppy controller: one internal 1.4 MB SuperDrive, one external 800 KB or 1.4 MB drive; the external port does not support the 400 KB drive, though it supports 400 KB disks in an 800 KB drive [1] pp. 4, 8, [2] ch. 1, 5 | [iwm-floppy.md](../../hardware/iwm-floppy.md) |
| ASC | four-voice sound synthesis; fed by two Sony sound chips that filter the pulse-width-modulated output and drive the speaker or external jack [2] ch. 5 | [asc.md](../../hardware/asc.md) |
| SCSI controller | NCR 53C80-compatible; internal 50-pin and external DB-25 connectors; pseudo-DMA handshaked by the MDU (§2.2) [2] ch. 5, [3] p. 393 | [ncr-5380.md](../../hardware/scsi/ncr-5380.md) |
| SCC-compatible serial | two Macintosh 8-pin DIN serial ports, each programmable for asynchronous, synchronous or AppleTalk protocols [1] p. 4, [2] ch. 5 | [scc.md](../../hardware/scc.md) |

### 4.2 Devices one board carries

| Device | Machine | Role | Page |
|---|---|---|---|
| Motorola MC68882 FPU | IIci (standard), IIsi (on either expansion adaptor) [1] Table 1-1 p. 9, [2] ch. 1 | floating point; IIsi software must probe for it, never assume it | — |
| PGC (Parity Generator Chip) | IIci, special order | parity over 9-bit SIMMs; enabled at startup only if the PGC and 9-bit SIMMs are present; error raises /NMI and /PARERR [1] pp. 21, 27–28, [3] p. 222 | §6.1 |
| Cache card + connector | IIci | 120-pin processor-direct cache slot; the card is physical, direct-mapped, controlled by RBV strobes and ROM traps [1] pp. 31–33, [rbv.md](rbv.md) §3.8, §4.6 | §6.1 |
| Combo chip | IIsi | integrates the SCC (85C30-compatible) and SCSI (53C80-compatible) functions in one part, "completely software compatible" with both [2] ch. 5 | [iisi.md](iisi.md) §10 |
| Egret microcontroller | IIsi | 68HC05-class part owning ADB, RTC, PRAM, soft power, power-on reset, keyboard reset and NMI; RTC/PRAM access is by modified ADB-style commands, so software that drove the old RTC chip directly does not work [2] ch. 5, [4] p. 58 | [egret.md](egret.md) |
| Sound-input circuit | IIsi | input jack, filter/preamplifier, FIFO and control logic; 8-bit monaural digitizing, interrupt-driven [2] ch. 5 | [iisi.md](iisi.md) §12.2 |
| Discrete RTC | IIci | the custom real-time clock behind VIA1 ([1] Figure 1-2 p. 7; [3] p. 77) — the II-family scheme the IIsi's Egret replaces | [rtc.md](../../hardware/rtc.md) |
| ADB transceiver chip | IIci | the ADB interface VIA1 drives ([1] Figure 1-2 p. 7; [4] Table 1.4) | [adb.md](../../hardware/adb.md) |

### 4.3 What the family does not carry

The roster's negative half matters as much as its positive half. Neither board has: a second
physical VIA (the RBV's emulation is the whole of VIA2, [rbv.md](rbv.md) §4.1); an external PMMU or
HMMU (the 68030's on-chip MMU is the only memory management, saving the II's external-PMMU wait
state [1] p. 15, [2] ch. 2); any IOP or intelligent peripheral processor; a SCSI DMA engine (SCSI
is pseudo-DMA through the MDU's DRQ handshake, §2.2); built-in Ethernet; or, on the IIsi, a
mathematical coprocessor. The IIci carries a cache *connector*, not cache RAM — the memory is the
card's, and the system "does not make any assumptions about the card's organization" [1] p. 33.

## 5. Interrupt, bus, and clock architecture

### 5.1 Interrupt levels

The family's interrupt architecture is the RBV's, and the RBV page carries it: the RBV is the
interrupt concentrator, priority is fixed, interrupts are always auto-vectored, and the level
assignment is VIA1 at level 1, the RBV's own VIA2 emulation at level 2, the SCC at level 4, and NMI at
level 7 — with the IIci's parity circuits able to ride level 7 and the IIsi's NMI raised by its
microcontroller rather than a switch ([rbv.md](rbv.md) §3.1, [4] Figure 2-1 p. 16). Two facts are
family-level:

- The built-in video's vertical blanking is delivered as a **slot-style interrupt**: "the built-in
  video generates an interrupt that is handled like a NuBus interrupt" [3] p. 62 — it arrives
  through the RBV's slot-interrupt machinery, not through VIA1's 60.15 Hz line ([rbv.md](rbv.md)
  §3.2).
- VIA1 keeps the II-family timer role: its CA1 receives the 60.15 Hz vertical-blank request and its
  CA2 the one-second tick from the RTC, on both boards [4] pp. 63–64 — the ticking the whole
  Macintosh toolbox clock stack is built on.

### 5.2 Slot and device interrupt paths

Each expansion slot contributes one interrupt line to the RBV's slot-interrupt register, which ORs
the lines into the level-2 aggregate and records the individual line states for the level-2 handler
to poll ([rbv.md](rbv.md) §3.2). The SCSI controller's IRQ and DRQ lines and the ASC's interrupt are
RBV interrupt-flag bits — on the IIci "both the IRQ and DRQ signals from the 5380 are stored in the
Interrupt Flags register in the VIA2 portion of the RBV" [3] p. 393, and the ASC's bit 4 is
[asc.md](../../hardware/asc.md) §6. On the IIsi, the PDS connector carries three /IRQ inputs
(/IRQ1–/IRQ3) alongside /TM0A and /TM1A [2] Table 6-1, so a processor-direct card can behave like a
pseudo-slot device [5] ch. 15; the Egret's requests reach the processor as NMI/keyboard reset rather
than through the RBV [2] ch. 5.

Shipped multi-machine kernels treat the two boards as one class: the m68k kernel headers dispatch
both the IIci and the IIsi to the same RBV register-offset set (rIFR at $1A03, rIER at $1C13) against
the VIA2 base, distinct from the real-6522 path of the II/IIx/IIcx/SE/30 [6].

### 5.3 Bus mastership and arbitration

The 68030's three-line arbitration (/BR, /BG, /BGACK) governs every bus master on both boards. The
documented masters are: the 68030 itself; a cache card on the IIci, which supplies data in place of
RAM by asserting CACHE at /AS (§2.4) and never while another master owns the bus (it must watch
BGACK~, with the pull-up and double-rank synchronization the IIci note mandates [1] p. 34); a PDS
card on the IIsi, whose RAM cycles as master are /STERM-terminated bursts (§2.4); and the NuChip30,
which acquires the processor bus on behalf of a mastering NuBus card — the IIci's cache-timing
appendix draws the sequence: NuBus controller /BR, /BG, /BGACK, bus acquisition, the DRAM access,
then "bus release by NuBus (waits for NuBus ACK)" [1] Figure C-2 p. 78. Card-to-card NuBus transfers
never reach the CPU bus at all, which is why the operating system marks NuBus space non-cacheable
[1] p. 34, [3] p. 224.

NuBus-to-RAM traffic can be blocked by software through the RBV's bus-lock bit — the II-family
mechanism "used to protect time-critical operations from interruption by NuBus transactions",
answered on the NuBus side with a try-again-later response ([rbv.md](rbv.md) §3.3, [3] p. 175).

### 5.4 The bus-error paths

The MDU's transfer monitor (§2.2, §3.8) is the family's only hardware error generator: it halts the
processor with /BERR on a transfer that fails to complete — the $F0xx window from the CPU side
(§3.4), the SCSI pseudo-DMA timeout (§3.8), and any cycle a cache card aborts for a retry (§2.4).
NuBus transaction errors take the parallel path: bus error to the processor, error type recorded by
the RBV on /TM0A//TM1A for software ([rbv.md](rbv.md) §3.3). The timeout durations are not printed
(§7).

### 5.5 Clocks

The family's clock tree has two roots, and which chip owns which frequency is family architecture:

| Clock | Frequency | Generated by | Consumers |
|---|---|---|---|
| CPUCLK | 25 MHz (IIci) / 20 MHz (IIsi) | the MDU [3] p. 115 | the 68030, and the MDU's own state machine ([1] Appendix A p. 63) |
| E clock | 783.36 kHz | the MDU [3] p. 115 | VIA1 (its timers pace on it), and thereby the RTC/ADB bit-banging |
| 15.6672 MHz (C16M) | 15.6672 MHz | the RBV [rbv.md](rbv.md) §1.6 | the bus-error and RAM-refresh circuitry; the SCC's rate reference, by division |
| 3.672 MHz | 3.672 MHz | the RBV [rbv.md](rbv.md) §1.6 | the ADB transceiver (IIci) and the SCC rate setting |
| Dot clocks | 30.2400 / 57.2832 / 15.6672 MHz | the RBV, selected by monitor sense ([rbv.md](rbv.md) §3.6) | the video shift register and VDAC |

The MDU's CPU clock is exported to expansion: the IIci's cache connector carries CPUCLK (25 MHz) on
pin C38 [1] Table 4-4 p. 37, and the IIsi's expansion connector carries CPUCLK (20 MHz) on A38,
**C16M** ("15.6672 MHz gen clock") on C38 and ECLK on B38 [2] Table 6-1 — the three timebases a PDS
card needs. The SCC reference nuance is that the Hardware Overview's comparison tables list the
IIci's serial ports as an "8 MHz 8530 SCC" against the 4 MHz part of the II/IIx/IIcx [4] Table 1.4;
the exact PCLK arrangement behind that entry is not printed anywhere else (§7).

### 5.6 Reset distribution

Reset sequencing is board architecture even though the sources differ: the IIci's Sony sound IC
holds /RESET 0.25 s after power stabilizes (§2.5), and the IIsi's Egret asserts Test (short time
constant, resets the MDU first) and Reset (long time constant, reaches the 68030 and the other
devices) [2] ch. 5. /RESET reaches the expansion slots on both boards — on the II-family contract
"the /RESET signal is also available to each expansion card slot so that expansion cards can be
reset to a known initial state" [3] p. 244, and the IIsi's connector carries an open-collector /RESET
the card may also drive [2] Table 6-1, 6-2. The MDU itself comes out of reset first on the IIsi
(§2.5); nothing printed states a separate MDU reset on the IIci beyond the system /RESET (§7).

## 6. Per-machine index

### 6.1 Macintosh IIci — [iici.md](iici.md)

The senior board: 25 MHz 68030 with a 25 MHz 68882 as standard equipment [1] Table 1-1 p. 9, 1–128 MB
of 80 ns fast-page DRAM in two interchangeable four-SIMM banks [1] p. 21, and 512 KB of ROM in a
socket arrangement the comparison table lists as "DIP & SIMM" [1] Table 1-1 p. 9. Its deltas against
this page: **three NuBus connectors** at geographic $C–$E (§3.4) with the II-family card power budget
(2.0 A at +5 V, 0.175 A at +12 V, 0.15 A at −12 V per card) [1] Table 4-5 p. 39; the **120-pin cache
connector** — not SE/30-compatible, 5 W budget, CARD-asserted CACHE stealing cycles from the MDU
(§2.4), card-decoded data/tag windows, ROM-trap control ([rbv.md](rbv.md) §4.6); the optional **PGC**
parity system: 9-bit SIMMs, parity written on every RAM write when the PGC is present, checking
enabled at startup only when PGC and 9-bit SIMMs are both found, VIA1 Data register B bit 6 the
enable, /NMI plus /PARERR the error outputs, and the pre-enable obligation to write valid parity to
all of RAM first [1] pp. 21, 27–28, [3] p. 222 — with the cache-timing appendix adding that parity
errors are not generated for writes or for accesses outside DRAM physical space $0000 0000–$07FF
FFFF [1] Figure C-1 note p. 77; and the classic II-family companion set — discrete RTC and ADB
transceiver behind VIA1 [1] Figure 1-2 p. 7, programmer's-switch NMI through the RBV
([rbv.md](rbv.md) §5), soft power with the trickle-charged capacitor and lockable rear switch
(§2.5). Machine page: [iici.md](iici.md).

### 6.2 Macintosh IIsi — [iisi.md](iisi.md)

The cost-reduced board: 20 MHz 68030, **no FPU as standard** — software must probe for it, and the
20 MHz 68882 arrives only on an expansion adaptor [2] ch. 1. Deltas: **bank A soldered down** (1 MB,
eight 256K×4 parts) with only bank B socketed, 100 ns DRAM, 65 MB maximum [2] ch. 3; the **Combo
chip** integrating SCC and SCSI [2] ch. 5; the **Egret** microcontroller owning ADB, RTC, PRAM, soft
power, power-on reset, keyboard reset and NMI — no programmer's switches; Command-power is NMI and
Command-Control-power is a hard reset, both held at least one second, with NMI off by default until a
control panel enables it [2] ch. 5; **one 120-pin expansion connector** whose user-installable
adaptor kit selects either a processor-direct slot (SE/30-compatible electrically, but /STERM bus
mastering, §2.4) or a single geographic-$9 NuBus slot carrying the NuChip30 on the adaptor card
itself [2] ch. 6; **built-in sound input** [2] ch. 5; and the video variant of §3.4's slot map — the
frame buffer rides super-slot $E, and the 12" RGB 512×384 mode is live on the IIsi's sense code 010
where the IIci reserves it ([rbv.md](rbv.md) §3.5, §5, [iisi.md](iisi.md) §8.1). Machine page:
[iisi.md](iisi.md).

### 6.3 Designs that did not ship, and the family's edges

Apple's internal comparison table of February 1991 lists **Buccaneer** — 40 MHz 68030, MDU memory
controller, RBV video with a VIA2 of RBV, NuChip30, Egret — with no introduction date, between the
shipped IIfx-class machines and the 68040 projects [4] Table 1.5. Whether any product shipped from
that project is not established by the printed record (§7). On the family's other edges: the LC's V8
is a register-compatible but genuinely different design (VRAM-based video, a VIA cell, integrated
sound) — [rbv.md](rbv.md) §1.5; the interrupt-priority figure groups the IIci with "Tim", the
fixed-display designs of the Classic II class [4] Figure 2-1 p. 16, whose video role is
[video-overview.md](../../hardware/video-overview.md) §3.4's subject; and the RBV's register layout
outlives the chip into the Power Macintosh AMIC ([rbv.md](rbv.md) §5).

## 7. Open questions

1. **The IIci's I/O sub-decode.** No Apple document prints it; the window table of §3.3 is the IIsi's,
   and the IIci's is inferred from it, from the shared device set, and from the PDM AMIC's later
   documented reuse of the same layout ([rbv.md](rbv.md) §6, item 1). A printed IIci decode — or a
   logic-analyzer capture of a IIci board — would close the family's biggest hole.
2. **Does the MDU have any software-visible register at all?** Neither developer note describes one;
   the *MDU User Manual* Apple's own reference list names [4] p. 72 is not in this evidence set.
   Whether the part carries test or configuration registers behind some address is unknown; every
   documented behaviour is decode, acknowledge, refresh, clock and overlay.
3. **The power-up map's mirror structure below $4000 0000.** The text states only that ROM is
   selected for all of $0000 0000–$3FFF FFFF; the figure's internal boundary labels for the
   duplicated images are not recoverable from the printed figures in this corpus. Whether the low
   gigabyte is one 512 KB image tiled uniformly or a ladder of growing aliases (as the true ROM
   window is) is unestablished.
4. **The ROM alias ladder in $4000 0000–$4FFF FFFF.** Both notes' figures mark boundaries
   ($4008 0000, $4010 0000, $4020 0000, and higher) that only partially survive into readable text;
   the ladder tabulated at [iisi.md](iisi.md) §3.2 is one reading of the IIsi figure. The IIci's
   ".5–32 MB" ROM subsystem [1] Figure 1-2 p. 7 implies a 32 MB top alias, but the printed boundary
   addresses above $4020 0000 are not reliably legible in either figure.
5. **The overlay release's first access.** The map flip is triggered by the first access to
   $4000 0000–$4FFF FFFF (§2.6), but which instruction performs it — and therefore which alias of
   the ROM the IIci's reset vectors point into — is ROM-image behaviour for which no annotated IIci
   or IIsi ROM disassembly exists in this evidence set. The machine pages' boot sections carry the
   burden until one does.
6. **Whether any VIA1 bit can force the overlay on the MDU machines.** The GLUE machines release the
   overlay through VIA1 PA4 [3] p. 244; on the MDU machines PA4 is a CPU-ID strap [4] pp. 63–64 and
   the MDU "performs ROM overlay automatically" [3] p. 153. Whether the overlay can be re-imposed in
   software at all (as some GLUE-machine diagnostics do) is not printed.
7. **The SCSI pseudo-DMA timeout duration.** "Different for different machines" is all the Guide
   says [3] p. 393; no source gives the IIci's or IIsi's figure, or whether the MDU distinguishes a
   hung bus from a slow device.
8. **Undecoded-address behaviour.** Beyond the $F0xx window (documented: /BERR) and the SCSI timeout
   (documented: /BERR), what a processor access to reserved RAM space, reserved ROM space or a
   reserved I/O sub-window does — /BERR, silent timeout, or an acknowledged bus-width read of
   undefined data — is stated nowhere. The Guide's generic statement that the MDU acknowledges
   devices "that are present" [3] p. 115 implies reserved space is not acknowledged, but the error
   shape is unconfirmed.
9. **The 60.15 Hz /VBLK source.** VIA1 CA1 receives it on both boards [4] pp. 63–64, but no source
   names its generator — the RBV is the natural candidate given its clock and video roles, and the
   print is silent.
10. **The "8 MHz 8530 SCC" entry.** The Hardware Overview's IIci and IIsi columns list the serial
    ports at 8 MHz against the II/IIx/IIcx's 4 MHz [4] Table 1.4; no other document states the SCC's
    PCLK on this family, and the RBV's 3.672 MHz rate-setting clock [rbv.md](rbv.md) §1.6 does not
    by itself explain the figure.
11. **The IIsi's RBV power-off bit.** The Egret owns the IIsi's supply (§2.5), so whether the RBV's
    PWR.OFF~ bit gates anything on that board is undetermined — carried open in the RBV page's
    open questions ([rbv.md](rbv.md) §6, item 10).
12. **The Overview's 512×384 entry for the IIci.** Table 1.4 credits the IIci's RBV with a
    512×384×1,2,4,8 mode [4] Table 1.4, but the IIci Developer Note's MON.ID table marks sense code
    010 "RESERVED for use by Apple" [1] Table 5-1 p. 45 — the code the IIsi uses for that mode
    ([rbv.md](rbv.md) §3.5). Whether later IIci ROMs enabled it, or the draft table is simply wrong,
    is unresolved.
13. **The FD0–3A / FD0–3B device-select labels.** The IIci block diagram names the MDU's
    device-select outputs this way, grouped toward the RAM/video side and the I/O side [1] Figure 1-2
    p. 7 (*observed* in the diagram), but no text explains the grouping or the full set of devices
    each output strobes.
14. **The MDU's reset on the IIci.** The IIsi's Test signal resets the MDU ahead of the processor
    [2] ch. 5; whether the IIci's single /RESET net gives the MDU any earlier or separate release is
    not printed.
15. **Egret and Combo part documentation.** The Egret's behaviour is summarized in the Hardware
    Overview [4] pp. 58–59 but its full specification (the packet protocol, the pseudo-device
    commands) is in Apple ERS documents not in this evidence set; the same is true of the Combo
    chip's part number. Their landing pages are stubs today ([egret.md](egret.md)); their open
    questions live there once written.

## References

1. Apple Computer, Inc., *Macintosh IIci Developer Notes*, Developer Technical Publications, 1989.
   Features and architecture ch. 1 pp. 3–9 (MDU/RBV architecture and feature list pp. 3–5, system
   comparisons Table 1-1 p. 9, NuChip30 history p. 8); hardware block diagram Figure 1-2 p. 7
   (device address/data wiring, device selects, RAA/RAB bank addresses); address mapping ch. 2
   pp. 13–17 (overlay and address space p. 13, physical maps Figure 2-1 p. 14, MMU p. 15, 24-bit
   mapping Table 2-1 p. 16); RAM interface ch. 3 pp. 21–28 (bank rules and wrap sizing p. 21, RAM
   configurations Figure 3-1 p. 22, use of RAM by video p. 23 and Figure 3-2 p. 24, DRAM
   specification Table 3-1 p. 24, SIMM pinouts pp. 26–27, PGC pp. 27–28); cache connector ch. 4
   pp. 31–42 (design rules and SE/30 incompatibility p. 31, CACHE and BGACK~ p. 33–34, address
   space Table 4-1 p. 32, traps Table 4-2 p. 33, signals Table 4-3 pp. 34–36, pinout Table 4-4
   pp. 36–37, power Table 4-5 p. 39); video interface ch. 5 pp. 45–54 (MON.ID values Table 5-1
   p. 45); NuBus interface ch. 6 p. 59 and Table 6-1 p. 59; RAM and ROM timing Appendix A pp. 63–65
   (MDU states and CPUCLK note p. 63, Figures A-1–A-5); diagnostic and emulator-support signals
   Appendix B pp. 69–73 (CPUDIS and emulator use p. 69); cache/parity/NuBus interactions Appendix C
   pp. 77–80 (Figure C-1 note p. 77, Figure C-2 p. 78).
2. Apple Computer, Inc., *Macintosh IIsi Developer Note*, 1990. Chapter 1 (features, FPU policy,
   compatibility); chapter 2 Address Mapping (overlay, physical maps Figure 2-1, 24-bit mapping
   Table 2-1); chapter 3 The Memory Interface (bank A/B layout and wrap sizing, DRAM Table 3-1,
   refresh, ROM interface: 512 KB SIMM then soldered device, five-clock access, no burst, 64-pin
   ROM SIMM Table 3-3); chapter 4 The Video Interface; chapter 5 Input/Output Interfaces (I/O
   address map Figure 5-1, Combo SCC/SCSI, SWIM, VIA interface, sound output and input, ADB
   microcontroller, power control, power-on reset: Test and Reset, keyboard reset and NMI, network
   booting, programmable wakeup); chapter 6 Expansion Interface (PDS and NuBus adaptor kits,
   connector signals Tables 6-1/6-2, /STERM bus-master rule, NuChip30 on the adaptor); chapter 7
   Software Overview (universal ROM).
3. Apple Computer, Inc., *Guide to the Macintosh Family Hardware*, 2nd ed., Addison-Wesley, 1990.
   IIci interrupts ch. 2 p. 62; RAM bank-A sharing pp. 60–63; general-logic IC survey p. 65;
   machine component table p. 88; IIci address map section pp. 139–141 and NuBus translations
   Table 3-10 pp. 139–140; VIA1 functions and the IIci overlay note p. 153; MDU and RBV component
   descriptions ch. 3 pp. 115–117; VIA2/transfer-mode bits p. 156; v2BusLk p. 175; IIci video RAM
   and the bank mapping p. 221; PGC parity p. 222; cache-card operation p. 224; power-up and
   startup sequence ch. 6 p. 244; power down p. 245; RTC description p. 77; SCSI handshaking and
   the MDU note p. 393; screen-buffer allocation note p. 412.
4. Apple Computer, Inc., *Macintosh Hardware Overview*, revision 2, 11 February 1991. MDU-family
   MMU note p. 8; interrupt priorities Figure 2-1 p. 16 and cache notes p. 16; machine comparison
   tables: high-end Macintoshes Table 1.4 pp. 9–10 (the IIci column; the IIsi appears as "Mac
   IIsl") and Table 1.5 p. 10 (Buccaneer); VIA/RBV bit assignments Appendix 1 pp. 63–64; Egret
   (68HC05) summary pp. 58–59; reference lists pp. 71–72 (MDU User Manual, PGC User Manual, RBV
   spec 343S1019).
5. Apple Computer, Inc., *Designing Cards and Drivers for the Macintosh Family*, 3rd ed.,
   Addison-Wesley, 1992. IIsi 32-bit physical address spaces Table 15-10 p. 344; IIsi expansion
   signals and the /STERM bus-master rule ch. 15 pp. 344–361; Macintosh IIci cache connector
   ch. 23 pp. 519–528 (diagnostic/emulator signals pp. 519–521, signal descriptions Table 23-3
   pp. 521–523).
6. Linux kernel, m68k Macintosh VIA/RBV register definitions, `arch/m68k/include/asm/mac_via.h` —
   the machine-class dispatch that routes the IIci and IIsi to the RBV offset set (rIFR $1A03,
   rIER $1C13) against the real-6522 offset set of the II/IIx/IIcx/SE/30.
