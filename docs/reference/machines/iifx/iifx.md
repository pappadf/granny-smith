# The Macintosh IIfx

The Macintosh IIfx (1990) is Apple's high-end 68030 Macintosh: a 40 MHz MC68030 and
MC68882 supported by a cluster of Apple custom integrated circuits that no earlier
Macintosh carries — two I/O Processors (IOPs) that own the serial, floppy and ADB
interfaces, a programmable interrupt controller (the OSS), a bus-master SCSI DMA chip
wrapping an NCR 53C80 cell, the Fast Memory Controller (FMC) behind a 32 KB board-level
cache, and a three-chip NuBus bridge (BIU30, BIU2, CGTO). This page is the machine page
of the `iifx/` family and, because the machine has no siblings, it also carries the
**family role** (§1.1): the board architecture, the memory map and address decode, the
device roster and the interrupt, bus and clock architecture of the whole family live
here, while register-level detail lives in the family's three device pages —
[pic.md](pic.md) (the OSS interrupt controller), [iop.md](iop.md) (the two IOPs, their
register windows, mailbox protocol and front-side sub-protocols) and
[scsi-dma.md](scsi-dma.md) (the Apple SCSI DMA chip) — and in the cross-machine hardware
pages for the parts the IIfx shares with the rest of the Macintosh line.

**Contents:**

1. [Identity & family role](#1-identity--family-role) — the family-of-one statement, model identity
   (VIA1 CPU.ID, ROM identity, the ROM's machine detect), specification summary
2. [Board architecture](#2-board-architecture) — the fast/slow subsystem split, the custom IC set,
   main memory and its parity option, the RAM cache, ROM, sound and video
3. [Memory map & address decode](#3-memory-map--address-decode) — top-level map, the I/O island and
   its $40000 mirroring, acknowledge/timeout/bus-error behaviour, the ROM overlay
4. [Device roster](#4-device-roster) — every chip on the board, its Apple part number, its I/O window
   and its OSS interrupt source
5. [Interrupt, bus, and clock architecture](#5-interrupt-bus-and-clock-architecture) — the OSS
   interrupt model, bus masters and DMA, the clock set, reset and power control
6. [Expansion](#6-expansion) — the six NuBus slots and their bridge chips, the 120-pin 68030
   processor-direct slot, pseudoslot design
7. [Boot sequence summary](#7-boot-sequence-summary) — reset vectors, the ROM's machine detect, POST
   phases, and what the OS does with the result
8. [Open questions](#8-open-questions)

---

## 1. Identity & family role

### 1.1 A family of one

The `iifx/` family contains exactly one machine. Under this tree's three-level
documentation rule a family page exists to hold what every member of a family shares; a
family of one therefore folds that role into its single machine page, and this is that
page. The practical consequence for a reader is a placement rule, not a summary rule:
everything board-level — the address map, the device roster, the interrupt and clock
architecture, expansion, and the boot contract — is stated here once; the three custom
parts with a real software surface are each documented to the bit in their own device
pages and are cited here by section, never restated. Nothing in this page or the three
device pages depends on how the machine is modelled by any particular implementation.

The split of duties inside the family is:

| Level | Page | Holds |
|---|---|---|
| Family + machine (this page) | iifx.md | board architecture, memory map, device roster, interrupt/bus/clock architecture, expansion, boot |
| Device | [pic.md](pic.md) | the OSS interrupt controller: register file, source map, autovectoring, POST self-tests |
| Device | [iop.md](iop.md) | the two IOPs: host register window, shared-RAM mailbox protocol, SCC IOP, SWIM IOP (SonyIOP, ADB), RPU probe |
| Device | [scsi-dma.md](scsi-dma.md) | the Apple SCSI DMA chip: register file, DMA/FIFO machinery, bus arbitration, watchdog, driver sequences |
| Cross-machine | [via.md](../../hardware/via.md), [scc.md](../../hardware/scc.md), [asc.md](../../hardware/asc.md), [swim.md](../../hardware/swim.md), [ncr-5380.md](../../hardware/scsi/ncr-5380.md), [rtc.md](../../hardware/rtc.md), [adb.md](../../hardware/adb.md), [declaration-rom.md](../../hardware/nubus/declaration-rom.md) | the parts the IIfx shares with other Macintoshes |

### 1.2 What the machine is

Apple's own summary of the machine is a feature list against the Macintosh IIx: a 40 MHz
MC68030 with internal caches and a built-in memory management unit; an MC68882
floating-point unit; the FMC controlling main RAM and 32 KB of on-board cache RAM; up to
128 MB of RAM in four or eight SIMMs; ROM on a SIMM; the OSS performing address decoding
and control; six NuBus slots with the NuBus interface provided by the BIU30, BIU2 and
CGTO custom ICs; a 120-pin processor-direct slot; an ASC sound chip with two Sony analog
ICs, a speaker and a stereo jack; one VIA; two IOPs — one for serial I/O and AppleTalk
through the 8530 SCC, one for the SWIM, the floppy interface and the ADB; an 8530 SCC;
the SWIM; one internal FDHD drive with provision for a second or an 800 KB drive; and
the SCSI DMA IC providing true direct-memory access for high-speed parallel devices
[1] pp. 32–33. A special-order model adds a custom PLD and 9-bit SIMMs for parity
generation and detection [1] p. 32, [1] p. 222.

The machine's project name inside Apple is **F19**: the theory-of-operation document for
its peripheral chips is titled "F19 — Theory of Operation" and the name appears on its
pages [4]. The internal hardware overview of February 1991 lists the machine's
introduction date as 3/90 [2] ch. 1, Table 1.4.

### 1.3 Identity: how software recognizes the machine

The machine is identified by hardware straps, not by a name plate. Three identification
mechanisms are established:

**VIA1 CPU.ID.** Four input bits of VIA1 Data register A are permanently wired high or
low on the main logic board to define the 4-bit model-identity code CPU.ID0–CPU.ID3, and
these are the *only* bits used in VIA1 Data register A on the IIfx [1] Table 4-7 p. 165;
the ROM reads them to determine which model it is running on [1] p. 154. The IIfx code
is [1] Table 4-8 p. 165:

| VIA1 DRA bit | Name | IIfx value |
|---|---|---|
| 6 | CPU.ID3 | 1 |
| 4 | CPU.ID2 | 1 |
| 2 | CPU.ID1 | 0 |
| 1 | CPU.ID0 | 1 |
| 7, 5, 3, 0 | — | reserved [1] Table 4-7 |

(For contrast, the Macintosh IIci reads 1, 0, 1, 1 on the same bits [1] Table 4-8.)

**The ROM's machine detect.** Very early in POST the ROM runs a probe sequence that
identifies the machine by two behavioral properties: the mirror period of the I/O space
and the bus-error behaviour of an undecoded window (§7.2). The IIfx settles the ROM's
candidate machine-ID byte at **$06**, matched against the per-machine entry record at
`$408037C0` in the ROM's entry table, which points at the IIfx per-machine descriptor
block at `$40803530` [5]. That descriptor is the ROM's own table of the IIfx's I/O
windows (§3.2) and is the authoritative in-ROM statement of the device roster.

**ROM identity.** The boot ROM is a 512 KB image on a 64-pin ROM SIMM containing four
1 Mbit ROM ICs [1] p. 232. The shipped image is dated 5 February 1990; its longword
checksum is **$4147DD77** (stored in the first four bytes of the image, which double as
the reset stack pointer), and its header carries version words $067C (offset $08) and
$11F2 (offset $12) [5]. The ROM SIMM socket accepts modules up to 8 MB [2] pp. 19–20,
[1] p. 486. The same ROM family ("Universal") serves several machines; the IIfx ROM
contains the per-machine entry table (§7.2) precisely so one image can cover many boards
[5].

The Toolbox-level machine identity that `Gestalt` and `SysEnvirons` report for the
IIfx is not established in the evidence corpus for this page (open question 1); the three
mechanisms above are what the boot path itself uses.

### 1.4 Specification summary

From the Guide's specification appendix [1] pp. 486–487, supplemented where noted:

| Attribute | Specification |
|---|---|
| Processor | MC68030, 32-bit internal architecture, 32-bit external data and address buses [1] p. 486 |
| Processor clock | 40.00 MHz [1] p. 486 |
| Coprocessor | MC68882 FPU at 40.00 MHz [1] p. 486 |
| Memory management | built into the MC68030 [1] p. 486 |
| RAM | 4 MB minimum, expandable to 128 MB on the board (plus up to 2 GB on NuBus cards); 80 ns RAS SIMMs [1] pp. 486, 211 |
| Board cache | 32 KB fast RAM cache [1] p. 486 |
| ROM | 512 KB, expandable to 8 MB [1] p. 486 |
| Parameter RAM | 256 bytes in the real-time clock chip [1] p. 486 |
| Sound RAM | 2 KB in the ASC [1] p. 486 |
| Floppy | one internal 1440 KB FDHD drive; one optional second internal FDHD drive [1] p. 486 |
| Hard disk | optional internal 80 or 160 MB SCSI drive; external SCSI connector [1] p. 486 |
| Video | separate monitor driven from a NuBus card; no built-in video [1] p. 486 |
| Serial | two RS-422 ports, synchronous modem support on one port, behind an IOP [1] p. 486 |
| ADB | two Apple Desktop Bus connectors [1] p. 486 |
| Expansion | six NuBus slots; 120-pin 68030 processor-direct slot [1] p. 486 |
| Sound | four-voice stereo or mono, 22.255 or 44.1 kHz sample rate [1] p. 486 |
| SCSI | one internal and one external connector, DB-25 [2] ch. 1 Table 1.4, [1] p. 486 |
| Power requirements | 100–240 V rms, 48–62 Hz; 230 W peak, not counting monitor or peripherals; 90 W maximum sustainable [1] p. 487 |
| Main unit | 10.9–11.8 kg, 140 × 474 × 365 mm [1] p. 487, Table A-9 |

---

## 2. Board architecture

### 2.1 Two subsystems and the speed-shift buffers

The IIfx's defining structural fact is that it is **two machines on one board, separated
by buffers**. The clock for the main processor is much faster than in other Macintosh
models, so the data and address buses to the I/O interfaces are buffered by fast/slow
buffers controlled by a PAL; most of the devices on the slow side of the bus operate at
half the main processor's clock speed. The speed-shift PAL provides exactly two
exceptions: it switches to the fast speed for processor-direct-slot accesses in the
address range $6000 0000–$6FFF FFFF, and for accesses to the MC68882 FPU [1] p. 61.

The expansion electrical specification quantifies the two subsystems. Timing comes
from an 80 MHz oscillator whose output is divided by 2 to give the **40 MHz CPU clock**
of the memory (fast) subsystem, and divided by 4 to give the **20 MHz clock** of the I/O
(slow) subsystem [3] p. 357. The 68030 Direct Slot sits on the I/O side of the buffers —
its CPUCLK runs at 20 MHz and its timing interface looks exactly like an MC68030
running at 20 MHz — yet a card that addresses the memory subsystem is answered at full
memory speed, because the memory controller's response time is constant; when the
processor accesses the card it slows down and synchronizes to the 20 MHz clock,
transparently to the card [3] p. 357. Which address ranges force the speed shift is
therefore a property of the address map (§6.2).

The internal overview summarises the same split in one table column: the IIfx is the
only Macintosh II-family machine of its generation with explicit "I/O buffering"
buffers and "I/O coprocessors" (the PICs — the IOP chips) for the SCC and SWIM [2] ch. 1
Table 1.4.

### 2.2 The custom IC set

The general logic of the machine is a mix of Apple custom ICs and PALs [1] Table 3-7
p. 111. The full roster with citations; register detail follows in §4:

| IC | Role | Part number | Documented |
|---|---|---|---|
| **OSS** (Operating System Support) | programmable interrupt controller, I/O address decode, DSACK generation, bus time-out, NuBus slot-interrupt recording, 60.15 Hz monitor, shutdown, parity path | 344S0076 (*reported — unverified*) | [pic.md](pic.md) |
| **FMC** (Fast Memory Controller) | DRAM controller, RAM cache controller, ROM control and overlay, burst access | not published (open question 2) | this page §2.3–§2.5 |
| **SCSI DMA** | 53C80-compatible SCSI cell plus a 68030-bus DMA engine | 343S0064 [2] p. 34 | [scsi-dma.md](scsi-dma.md) |
| **IOP × 2** (Peripheral Interface Controller) | 65CX02-based I/O processors: one for the SCC, one for the SWIM and ADB | 343S1021 [2] p. 33 | [iop.md](iop.md) |
| **BIU30**, **BIU2**, **CGTO** | the NuBus interface between the 40 MHz processor bus and the 10 MHz NuBus [1] p. 32 | BIU30 = 344S0074, BIU2 = 344S0075 [2] p. 31; CGTO unknown (open question 6) | this page §6.1 |
| **Speed-shift PAL** | fast/slow buffer control [1] p. 61 | — | this page §2.1 |
| **ASC** (Apple Sound Chip) | digital sound synthesizer, 2 KB sound RAM | 344S0063 on this machine — [asc.md](../../hardware/asc.md) §2.1 | [asc.md](../../hardware/asc.md) |
| **8530 SCC** | dual-channel serial controller, behind the SCC IOP | Zilog Z8530 [1] p. 33 | [scc.md](../../hardware/scc.md) |
| **SWIM** | floppy controller, behind the SWIM IOP | [1] p. 33 | [swim.md](../../hardware/swim.md) |
| **65C22 VIA** | the machine's only VIA: RTC interface, one-second tick, CPU.ID straps [1] p. 154 | 65C22 | [via.md](../../hardware/via.md) |
| **RTC** | real-time clock, 256 bytes parameter RAM [1] p. 486 | — | [rtc.md](../../hardware/rtc.md) |
| **RPU** (RAM Parity Unit, optional) | parity generation/checking on the special-order model [1] p. 222 | — | [iop.md](iop.md) §19 |

Of the carried-over parts, the ASC, SCC, SWIM, VIA and RTC behave as on the other
Macintosh II-family machines; the IIfx-specific differences are in *where they sit*
(behind the IOPs, on the OSS interrupt path) and are covered in §4 and §5.

### 2.3 Main memory

RAM is provided in four or eight 64-pin SIMMs — the IIfx is the only Macintosh II-family
machine that does not use the 30-pin SIMM [1] p. 209. The IIfx SIMM pinout is its own
(Table 5-7 in the Guide, [1] pp. 214–215) and differs from the family SIMM in three
load-bearing ways:

- **separate data-in and data-out buses**: each SIMM exposes D0–D7 (data input to the
  RAM ICs) and Q0–Q7 (data output from the RAM ICs) as distinct pins, rather than one
  bidirectional bus;
- **one write-enable per RAM IC** (/W0–/W7), rather than a single /WE;
- **parity pins on every SIMM**: /QB (reserved parity), a parity-check output Q, a
  write-wrong-parity input /WWP, and a parity daisy-chain pair PDCI/PDCO [1] Table 5-7
  pp. 214–215.

The latched input/output structure is what lets the FMC overlap the RAM read and write
half-cycles; the parity pins exist because the ninth bit of a 9-bit SIMM is wired
through the daisy chain so the RPU can record whether parity SIMMs are installed
[1] p. 222.

Configuration rules: the smallest SIMM usable is 1 MB; DRAMs must be 80 ns RAS or
faster; the SIMMs are organized as two banks of four [1] pp. 210–211. Unlike the rest of
the family, either bank may be left empty and, when larger SIMMs become available, they
may be installed in either bank [1] p. 216. Maximum is 128 MB [1] p. 486.

**The parity option.** A special-order model carries the RPU and 9-bit SIMMs. At
startup, system software determines whether the parity IC is installed, reads the RPU,
and decides whether to enable checking; bit 6 of VIA1 Data register B enables or
disables parity generation and checking. Before reading RAM with parity enabled,
software must first write normal parity to all available RAM. On each read the SIMM
circuits generate a parity bit and compare it with the stored bit; on a mismatch, with
parity enabled, the RPU records the error condition and the SIMM that caused it, and
sends **interrupt IRQ14** to the OSS. System software writes 7 into that source's
Interrupt Mask register in the OSS to make parity errors non-maskable, and the handler
clears the interrupt by writing 0 back [1] p. 222. The full interrupt-side behaviour is
[pic.md](pic.md) §2.6 and §4.5; the RPU's probe window is [iop.md](iop.md) §19.

### 2.4 The RAM cache

The IIfx carries 32 KB of high-speed cache RAM on the main logic board, controlled by
the FMC [1] pp. 67, 228. Its properties, all from the Guide's cache chapter:

- The cache data RAM is 20 ns static RAM and the processor reads it **with no wait
  states** [1] p. 228. A hit is served in two clock cycles; a miss costs four more
  cycles to read from main memory, during which the FMC loads the new data into the
  cache [1] p. 67.
- Organization: direct-mapped, 2000 lines of four longwords each; **only burst reads are
  cached**; writes are no-wait-state cycles that always update the cache at the same
  time main memory is updated [3] p. 358. The Guide states the write rule flatly:
  write operations to cached memory always update the cache, regardless of bus
  mastership or the state of the cache control, so the cache cannot hold stale data
  [1] p. 230.
- Fill: on a miss the FMC initiates a **burst read of four longwords** from RAM or ROM,
  stores the address of the group in the tag RAM, and sets a validity bit on successful
  completion; there is one tag per four-longword block, so tags allocate only on burst
  reads [1] p. 230. The FMC can burst from either RAM or ROM [1] p. 230.
- Effect: the cache gives the processor a maximum RAM access rate of **64.00 MB per
  second**; since the hit rate is over 90% of processor read operations, the average
  rate is 60.95 MB/s [1] p. 196. ROM accesses run at the same 64.0 MB/s [1] Table 5-9
  p. 231.
- Writes are always **buffered**: the FMC latches the data and terminates the
  processor's write cycle before the data reaches memory, so a write that would take
  six clock cycles occupies the processor for only two; if the succeeding operation is
  anything other than another write it can proceed while the FMC completes the write
  [1] p. 230. For successive writes the FMC compares the latched address of the first
  write with the address of the second, and if they are in the same RAM page it performs
  the second write as a **page-mode write** with no wait states [1] pp. 230–231.
- The memory subsystem supports the 68030's on-chip cache burst protocol, and a
  processor-direct-slot card can use /CBREQ to request four longwords in succession;
  /CBREQ and /CIOUT are mutually exclusive, so the cache cannot be inhibited during
  burst cycles [3] p. 358.
- The 68030's own caches are the standard 256-byte instruction and 256-byte data
  caches; the IIfx (like the IIci) allows **burst reads to fill them**, where the IIx
  and IIcx use single-entry fills [2] p. 16.

The cache is logically tied to main memory at all times, which is why the internal
overview notes it "provides no opportunities for cache coherency problems" [2] p. 16 —
but see §5.2 for the other side of that statement: bus masters exist on this machine,
and the 68030's *on-chip* data cache is not snooped.

### 2.5 ROM

The 512 KB ROM image lives on a single 64-pin ROM SIMM holding four 1 Mbit ICs [1]
p. 232, in a socket that accepts up to 8 MB [2] pp. 19–20. The ROM occupies the usual
Macintosh ROM space at $4000 0000 [1] p. 141, with the image repeated throughout the
assigned space; the ROM's own descriptor gives its relocated base as $4080 0000 [5].
Access is by longword, all four bytes at once, as on the other 32-bit Macintoshes
[1] p. 68. At reset the FMC asserts a ROM overlay so that the reset vectors are fetched
from ROM (§3.4).

### 2.6 Sound and video

There is **no built-in video**. The IIfx uses a video expansion card in a NuBus slot and
an external monitor, like the Macintosh II, IIx and IIcx [1] p. 397; the ROM boots
blind and finds its boot screen through the Slot Manager's declaration-ROM scan
(§7.4). Everything screen-shaped on this machine is a NuBus card subject to §6.1.

Sound is the standard ASC circuit: the Apple Sound Chip (a discrete 344S0063 on this
machine — [asc.md](../../hardware/asc.md) §2.1) plus two Sony analog sound ICs filtering
the pulse-width-modulated output, a built-in speaker and an external stereo mini-phone
jack [1] pp. 32–33, 439–440. The F19 theory of operation describes the same circuit in
generation terms: the ASC's two 1024-byte FIFOs accept sound values in place of a RAM
address space, which "removes much of the time critical nature of sound generation and
gives stereo sound", and its four-voice mode is a hardware implementation of the
four-voice driver in the Macintosh ROMs [4]. All normal accesses to the ASC register
set are byte-wide, except the FIFO, which may be loaded with longword instructions since
the 68030 generates four byte accesses with incremented addresses [4]. The IIfx maps
the ASC at $50F1 0000 rather than the earlier machines' address — asc.md §3.3 — and the
ASC interrupt and the external-jack sense /SNDEXT arrive on the OSS rather than on a
VIA2 [1] pp. 157, 439.

---

## 3. Memory map & address decode

### 3.1 Top-level map

The MC68030 can directly access 4 GB of address space, divided into blocks for RAM,
ROM, NuBus slots and the I/O devices; in most respects the IIfx map is identical to the
Macintosh II map [1] p. 141. The coarse structure:

| Physical range | Contents |
|---|---|
| $0000 0000 upward | RAM, up to 128 MB in two banks [1] pp. 32, 486; the contents of RAM are repeated throughout unused RAM space as identical images [1] p. 121 |
| $0000 0000–$0007 FFFF | ROM overlay at power-on and reset (§3.4) |
| $4000 0000–$4FFF FFFF | ROM, 512 KB image repeated [1] pp. 141, 232; the ROM's relocated base is $4080 0000 [5] |
| $5000 0000–$5FFF FFFF | on-board I/O, the whole 256 MB repeating with the IIfx's own mirror period (§3.2) [1] p. 141, [5] |
| $6000 0000–$6FFF FFFF | processor-direct-slot "slow" space [3] p. 355; the speed-shift PAL runs the fast clock here [1] p. 61 |
| $7000 0000–$7FFF FFFF | processor-direct-slot "fast" space [3] p. 355 |
| $6000 0000–$FFFF FFFF | NuBus super slot space: a 32-bit address of the form $ssxx xxxx accesses super slot space for slot $s; slots $9–$E hold $1000 0000–$EFFF FFFF [1] p. 138 |
| $F100 0000–$F8FF FFFF | presently unused standard slot space [1] Table 3-10 p. 140 |
| $F900 0000–$FEFF FFFF | standard slot space, slots $9–$E: $Fsxx xxxx [1] p. 138 |
| $F000 0000–$F0FF FFFF | NuBus card to main logic board: RAM ($0000 0000–$3FFF FFFF), I/O ($F000 0000–$F070 FFFF → $5000 0000–$507F FFFF) and ROM ($F080 0000–$F0FF FFFF → $4080 0000–$40FF FFFF). A main-processor access here generates an immediate bus error and no NuBus transaction [1] Table 3-10 p. 140 |

A 24-bit address of the form $sx xxxx, with s in $9–$E, translates to $Fs0x xxxx, so only
the lower 1 MB of each card's standard slot space is addressable in 24-bit mode [1]
p. 139. An access to any address range to which no device is assigned results in a bus
error [1] p. 135.

NuBus cards reaching the main logic board are translated by the NuBus controller — on
this machine the BIU30 [1] pp. 139–140 — per the table above; a card can reach main
RAM at the same address, the I/O devices through the $F0xxxxxx window, and ROM through
the $F08xxxxx window [1] Table 3-10 p. 140.

### 3.2 The I/O island and its mirroring

The I/O devices of the IIfx live in the 256 MB I/O space at $5000 0000–$5FFF FFFF
[1] p. 141. Two facts distinguish the IIfx from every other Macintosh II-family
machine of its generation:

**The mirror period is $40000.** Every I/O window repeats every $40000 bytes through
the I/O space. The Macintosh II, IIx, IIcx and SE/30 mirror every $20000, and the boot
ROM's machine-detect probes exploit exactly this difference: a probe that sees a device
through a $20000 mirror concludes the machine is *not* an IIfx, while the IIfx
back-proves the negative — its $40000 mirror is visible and the $20000 retest fails
[5]. (The Macintosh IIci does not mirror I/O at all, which is why the detect keeps a
third, mirror-free probe path [5].)

**The canonical island is $50Fxxxxx.** The per-machine descriptor block in the ROM —
the table the boot reads its I/O bases from — gives the I/O base as $50F0 0000, and all
software-observed accesses run in the $50F0 0000–$50F2 7FFF island [5], [6]. The
windows inside one $40000 island, with the canonical addresses used by ROM and system
software:

| Window | Canonical base | Size | Contents |
|---|---|---|---|
| $50F0 0000 | offset $00000 | 8 KB | VIA1, sixteen registers spaced $200 apart [5]; also reachable at the next island position $50F4 0000 [5] |
| $50F0 4000 | offset $04000 | 8 KB | SCC IOP host registers, incl. the SCC bypass window at $50F0 4020 [5], [iop.md](iop.md) §2 |
| $50F0 8000 | offset $08000 | 8 KB | the SCSI DMA chip: the 53C80 cell's registers at +$000–$070 and the Apple DMA registers at +$080–$180, register select from A[8:4] — scsi-dma.md §2.1; observed as the chip base a shipping UNIX-family operating system programs its driver against [6] |
| $50F0 C000 | offset $0C000 | 4 KB | SCSI handshake (pseudo-DMA) read aperture [5] |
| $50F0 D000 | offset $0D000 | 4 KB | SCSI handshake (pseudo-DMA) write aperture [5] |
| $50F1 0000 | offset $10000 | 8 KB | ASC (Apple Sound Chip) [5]; byte-wide register set, FIFO longword-loadable [4] |
| $50F1 2000 | offset $12000 | 8 KB | SWIM IOP host registers, incl. the SWIM/ISM bypass window at $50F1 2020 [5], [iop.md](iop.md) §2 |
| $50F1 8000 | offset $18000 | 8 KB | NuBus bridge (BIU30) configuration window [5] |
| $50F1 A000 | offset $1A000 | 8 KB | OSS interrupt controller [5] — [pic.md](pic.md) §2 |
| $50F1 C000 | offset $1C000 | 8 KB | OSS extension: a 16-bit serial shift register exercised by POST (§7.3) [5] |
| $50F1 E000 | offset $1E000 | 8 KB | the optional RPU, when the parity feature is installed; the address is otherwise undecoded and every access bus-errors, which is how POST detects the parity option's absence (§7.3) [5], [iop.md](iop.md) §19 |
| $50F2 4000 | offset $24000 | 16 KB | undecoded: accesses here reliably take a bus error, and the ROM's detect uses exactly this as its machine probe (§7.2) [5] |

The per-machine descriptor that anchors this table, disassembled out of the ROM at
`$40803530`, holds: ROM base $4080 0000; I/O base $50F0 0000; ASC $50F1 0000; SCSI DMA
$50F0 8000; SWIM IOP bypass $50F1 2020; SCC IOP bypass $50F0 4020; OSS $50F1 A000; and
the two OSS-extension windows $50F1 C000 and $50F1 E000 [5]. The descriptor's VIA2 and
ASC-init fields are zero — the machine has no second VIA, and the ROM's generic
two-VIA/ASC bring-up helper writes to physical address 0 (harmlessly, into RAM) rather
than to a device [5].

All I/O devices except the SCSI DMA chip are byte-wide, connected to the high-order
byte of the data bus (D31–D24) as in the rest of the family [1] p. 60; the SCSI DMA
chip is the one I/O device with a full 32-bit port ([scsi-dma.md](scsi-dma.md) §1.3).
The IOP windows are byte-staggered — each host register appears at two adjacent byte
addresses within its aperture ([iop.md](iop.md) §3).

### 3.3 Acknowledge, time-out and bus-error behaviour

The OSS performs the address decoding that maps the I/O devices into the processor's
address space and generates the acknowledge signals DSACK0 and DSACK1 for the I/O
spaces [1] p. 119. The OSS also carries the board's bus time-out logic [1] p. 119; for
expansion-card designers the observable contract is an overriding timer that generates
a /BERR any time /AS is asserted longer than **16 µs** [3] p. 355. Which accesses the
OSS acknowledges and which time out is fixed by its decode (§3.2); the reliable bus
error in the $50F2 4000–$50F2 7FFF window is load-bearing for the boot (§7.2) and the
reliable bus error at $50F1 E000 (no RPU) is load-bearing for the parity probe (§7.3)
— both are *expected* faults that the ROM catches through its minimal bus-error
handler [5].

### 3.4 The ROM overlay and 24-bit mode

Each Macintosh uses two address maps. The ROM overlay address map, used at power-on
and reset, maps the lowest addresses to ROM instead of RAM, because an MC68000-family
processor always takes its reset vectors from address $0 [1] p. 120. On the IIfx the
overlay covers **$0000 0000–$0007 FFFF** — the size of the 512 KB ROM — and is asserted
by the FMC [5]. The startup or Reset handler software switches to the normal address
map by setting the overlay signal low; on the two-VIA machines that signal comes from
VIA1, but the IIfx's VIA1 carries no overlay bit ([1] Table 4-7 p. 165, [1] p. 136),
and the overlay on this machine belongs to the FMC, whose software contract for
dropping it is not established in the corpus (open question 3).

In 24-bit mode the operating system uses only the low 24 bits of each address; the
68030's MMU ignores the high-order 8 bits and translates the resulting 24-bit address
into a 32-bit physical address for decoding. System software 7.0 and A/UX use the
on-chip MMU for full 32-bit addressing and virtual memory [1] p. 136. The ROM brings
the machine up with translation off: POST seeds the CACR with $2000 (both on-chip
caches disabled), clears the TC to zero, and only enables translation later in boot
[5] (§7.1).

---

## 4. Device roster

The table is the family-roster view: what each part is on *this* board, where it sits,
and where its bit-level documentation lives. Apple part numbers are given where a
corpus document states them.

| Device | I/O window | OSS source | Notes and detail page |
|---|---|---|---|
| MC68030 @ 40 MHz + MC68882 @ 40 MHz | n/a | n/a | [1] p. 486; FPU runs at full speed on the fast side of the buffers (§2.1) |
| FMC | none (decode only) | — | RAM, cache, ROM controller [1] p. 120; its one observed software surface is the OSS-extension serial register and the ROM-mirror invert flip-flop behind OSS ROM-control bit 3 (§7.3) [5] |
| OSS (344S0076, *reported*) | $50F1 A000 | — | the interrupt controller itself; [pic.md](pic.md) §2–§4 |
| VIA1 (65C22) | $50F0 0000 (+$50F4 0000 mirror) | 11 | RTC interface, one-second tick, CPU.ID straps — the machine's only VIA [1] p. 154, [5] |
| SCC IOP (PIC 343S1021 + external SRAM) | $50F0 4000 | 7 | owns the Z8530; direct 68030 access to SCC register space bus-errors [1] p. 110; [iop.md](iop.md) §2, §15 |
| Z8530 SCC | behind the SCC IOP ($50F0 4020 bypass) | — | 8 MHz part on this machine [2] ch. 1 Table 1.4; [scc.md](../../hardware/scc.md) |
| SWIM IOP (PIC 343S1021 + external SRAM) | $50F1 2000 | 6 | owns the SWIM and the ADB interface [1] pp. 109–110; [iop.md](iop.md) §2, §16–§18 |
| SWIM | behind the SWIM IOP ($50F1 2020 bypass) | — | clocked at C16M = 15.6672 MHz, twice the original Macintosh rate, so the internal divide-by-two is used for 800 KB drives [4]; [swim.md](../../hardware/swim.md) |
| ADB transceiver function | inside the SWIM IOP | — | the IOP's processor implements the ADB UART in software via its GPIN/GPOUT pins, eliminating the separate transceiver IC of earlier machines [1] p. 110, [2] p. 33, [iop.md](iop.md) §18 |
| Apple SCSI DMA (343S0064) | $50F0 8000 | 9 | wraps an enhanced 53C80 cell in a bus-master DMA engine; IRQ and DRQ are stored in the OSS interrupt flags [1] p. 393; [scsi-dma.md](scsi-dma.md) |
| ASC (344S0063) | $50F1 0000 | 8 | interrupt and /SNDEXT jack sense arrive on the OSS [1] pp. 157, 439; [asc.md](../../hardware/asc.md) §6 |
| RTC (+ PRAM battery) | via VIA1 PB0–PB2 | — | 256 bytes parameter RAM [1] p. 486; [rtc.md](../../hardware/rtc.md) |
| BIU30 (344S0074) / BIU2 (344S0075) / CGTO | $50F1 8000 config window | — | the three-chip NuBus interface [1] p. 32, [2] p. 31; register semantics unpublished (open question 6) |
| RPU (optional) | $50F1 E000 | 14 | parity; present only on the special-order model with 9-bit SIMMs [1] p. 222; [iop.md](iop.md) §19 |
| NuBus slots ×6 | $Fs00 0000 / $s000 0000 | 0–5 | one OSS source per slot, individually recorded [1] p. 157; §6.1 |
| 68030 Direct Slot (PDS) | $6xxx xxxx / $7xxx xxxx / pseudo-slot $E | /IRQ6, /IRQ15 | [3] pp. 355–356; §6.2 |

Three roster facts deserve prose because they are the ones software trips over:

**The ASC is not behind an IOP.** One IOP controls the SCC; the other controls the
SWIM and contains the ADB interface [1] p. 32. The sound chip is reached directly by
the 68030, with its own OSS source [1] p. 157.

**SCC register space is unreachable by the 68030.** In the IIfx an access by the MC68030
directly to the SCC register space causes a bus error [1] p. 110; serial drivers run
against the SCC IOP, either through its mailbox protocol or through the bypass window
that the IOP must first be asked to open ([iop.md](iop.md) §12, §15).

**The floppy path is IOP-mediated end to end.** The IOP that controls the SWIM also
controls the Apple Desktop Bus, eliminating the separate ADB transceiver [1] p. 110;
the SWIM itself is clocked at twice the original Macintosh rate and the drive-head
select line reaches the drives through the IOP path ([1] p. 155; open question 4 for a
cross-source conflict on exactly which chip drives the SEL line).

---

## 5. Interrupt, bus, and clock architecture

### 5.1 Interrupt architecture

Interrupts in Macintoshes are always auto-vectored; interrupting devices do not
respond to IACK cycles [2] pp. 14–15. On the IIfx the interrupt system is the OSS's,
and its defining property is that **priorities are software-assigned**: any of the 16
possible interrupt sources can be assigned to any 68030 priority level, so different
operating systems can use different interrupt priorities on the same hardware
[1] p. 63, [2] pp. 14–15. Every other Macintosh of the generation has fixed levels.
The internal overview's table of interrupt priorities marks the IIfx's row simply
"programmable" [2] p. 15.

The complete model — register file, source map, autovectoring, arbitration, sensing,
NMI paths and shutdown — is [pic.md](pic.md) §2–§4 and is not restated here. The
board-level facts that surround it:

- The sources fanned into the OSS are the six NuBus slot lines (individually recorded —
  the OSS ORs them into the slot interrupt and stores the number of the interrupting
  slot [1] pp. 156–157), the two IOPs' host-interrupt outputs, the Apple Sound Chip,
  the SCSI subsystem (the SCSI DMA's IRQ *and* DRQ are stored in the OSS interrupt
  flags [1] p. 393), VIA1, the 60.15 Hz vertical-blanking request, and the RPU's
  parity error (source 14) on parity machines [1] p. 222.
- VIA1 keeps only three jobs on this machine — RTC interface, the one-second interrupt
  from the RTC, and the CPU.ID straps [1] p. 154. Its timers and the one-second tick
  arrive at the OSS as VIA1's single source.
- The 60.15 Hz request, a VIA1 function on every other machine, is monitored by the OSS
  itself on the IIfx and is used by a variety of firmware and software [1] p. 155.
- Level 7 is reached by the programmer's interrupt switch — wired so as to be
  uninhibitable by the 68030's interrupt priority mask [1] p. 101 — and, on parity
  machines, by the RPU's error interrupt once software raises source 14's mask to 7
  [1] p. 222.
- The boot ROM programs an initial level assignment very early (§7.3); the system
  software's Start Manager then sets up the interrupt priority levels by writing the
  OSS control registers and initializing the interrupt vectors to match [1] p. 119.
  The observed boot-time assignment and its open conflicts are [pic.md](pic.md) §4.3.

An operating system that wants a different assignment simply rewrites the per-source
level registers; Apple's internal material expected precisely that for the IIfx's
alternative operating systems [2] pp. 14–15.

### 5.2 Bus masters and DMA

The IIfx is the first Macintosh II-family machine on which devices other than the
processor and NuBus cards hold bus mastership, so the arbitration order is documented
rather than implied. The bus-master priority scheme is [3] p. 356, Table 15-14:

| Priority | Bus master |
|---|---|
| First (highest) | 68030 Direct Slot |
| Second | NuBus |
| Third | SCSI |
| Fourth (lowest) | MC68030 processor |

The 68030 is the lowest-priority bus master, so the processor's performance degrades
as each expansion slot is filled [3] p. 356. Three master paths exist:

**SCSI DMA.** In master mode the SCSI DMA IC uses the normal bus-arbitration procedures
of the MC68030 to transfer data to and from main memory without assistance from the
processor [1] p. 395. Its arbitration, address multiplexing, cycle shapes, watchdog
and error paths are scsi-dma.md §3.3–§3.8. Apple's published 1990 position was that
Macintosh system software did not yet use the capability and that developers should
wait for it — scsi-dma.md §1.2, citing Apple Technical Note HW #09; an operating system that does
drive it has been observed to arm the engine in chunks of up to 32 KB per SCSI command,
walking a software scatter list, and to reach the chip's registers at $50F0 8000 and
its completion interrupt through the OSS level-2 dispatcher ([6]; scsi-dma.md §4.5).

**NuBus cards.** A card that wins NuBus arbitration reaches main-board RAM, I/O and ROM
through the BIU30 translation of Table 3-10 (§3.1) [1] p. 140. A PDS bus master cannot
read data from a NuBus expansion card at all — the expansion specification states this
flatly as a design constraint [3] p. 358.

**Processor-direct-slot cards.** A PDS card can take the bus as master through the
/CBREQ handshake; the card must complete its DMA cycle within the normal processor
cycle time and must assert /DS when addressing on-board devices [3] p. 358. The
NuBus and SCSI interfaces allow DMA access to the 68030 Direct Slot [3] p. 356.

**DMA and cache coherency.** The only time the processor has to share RAM access time
with any other device on this machine is when an expansion card has control of the bus
or when a DMA device is making DMA accesses [1] p. 67. The board-level cache cannot go
stale because writes update it under all circumstances (§2.4) — but the 68030's
on-chip data cache is *not* snooped, and on 68030-based machines other bus masters can
write into system memory already cached on the processor chip; the internal overview
points driver writers at the standard cache-coherency techniques for exactly this
[2] p. 16.

**IOP shared-RAM arbitration.** Each IOP's external static RAM is time-multiplexed
between the IOP's own processor and two DMA controllers on one side and the host on the
other; host and DMA accesses are performed during the unused ø2-low period of the
65CX02 clock, with fixed priority DMA channel 1, then DMA channel 2, then the host
[4] §1.2. The practical floor: with a generic DMA device asserting request always, the
host transfer rate falls to 1 MB/second; with the SCC receiving AppleTalk data at
230.4 kHz, the host rate is 1.97 MB/second against a best case of 2 MB/second [4]
§1.2. This arbitration is invisible to the host except as RAM-window latency.

### 5.3 Clocks

The master timing source is the 80 MHz oscillator of §2.1: divided by 2 for the 40 MHz
memory subsystem, divided by 4 for the 20 MHz I/O subsystem [3] p. 357. Downstream
clocks established from the corpus:

| Clock | Rate | Consumer | Source |
|---|---|---|---|
| CPU/memory clock | 40 MHz | 68030, FPU, FMC, DRAM | [3] p. 357, [1] p. 486 |
| I/O subsystem clock | 20 MHz | the slow side of the buffers, the PDS CPUCLK | [3] p. 357 |
| C16M | 15.6672 MHz | the SWIM (twice the original Macintosh rate; internal ÷2 for 800 KB drives) | [4] |
| IOCLK | 15.6672 MHz | the PIC/IOP state machines; divided down to the 3.6864 MHz serial clock | [4] §1.5 |
| ø2 (IOP core) | 2 MHz | the 65CX02 inside each IOP | [4] §1.0 |
| DPCLK | up to 20 MHz | the IOP digital phase-locked loops; ÷10 gives the 2 Mbit/s LocalTalk rate | [4] §1.2.1 |
| SCC clock | 8 MHz part | the Z8530 | [2] ch. 1 Table 1.4 |
| E clock | 783.36 kHz | VIA1, as on the rest of the family | [via.md](../../hardware/via.md); generator on the IIfx unnamed (open question 5) |
| 60.15 Hz | 60.15 Hz | the vertical-blanking request /VBLK, monitored by the OSS | [1] p. 155; origin on this machine unstated (open question 5) |
| 1 Hz | 1 Hz | the RTC one-second interrupt, via VIA1 | [1] p. 154 |

The DPLL block exists to support high-speed LocalTalk: the IOP recovers FM0 data and
converts it to NRZ for the SCC, with a carrier-sense detector that the internal
overview considers better at detecting packets than the SCC's hunt/missing-clock
convention; the enhanced features were not used by the system software [2] p. 33,
[4] §1.2.1.

### 5.4 Reset, power control and shutdown

**Hard power-on.** The machine has a soft-power design: AC can be turned on from an
ADB device (typically the keyboard, whose power-on key momentarily grounds ADB pin 2
to pin 4) or from a NuBus card through the PFW* line [2] p. 62, [4]. The power-control
circuit attempts turn-on while the power switch is pressed and for two seconds after;
the ADB connector pin 2 is the keyboard's secondary power switch [4].

**Shutdown.** When the user chooses Shut Down, the computer first closes all files and
finishes pending activity; when the software is ready it causes a control IC — VIA2 on
the two-VIA machines, the OSS on the IIfx — to assert the /POWEROFF signal, and the
power supply switches off within 2 ms [1] pp. 119, 245. On the IIfx the /POWEROFF bit
lives in the OSS's VIA2-emulation bank (bit 2 of its Data register B equivalent,
[2] Table 13.1 pp. 63–64); the F19 theory of operation describes the same flip-flop
circuit while still naming its source "VIA2" [4] — the register is the emulation bank
the OSS carries. The rear power switch generates a hard off that turns the machine off
after 2 ms without going through software [4]. A sustained reset brings the OSS to a
known state [1] p. 256.

**Processor reset.** The 68030's RESET instruction, executed as the first substantive
act of POST, asserts the /RESET line on the bus to reset the external devices before
bring-up [5] (§7.1).

**Supply.** The supply accepts 90–140 V rms and 170–270 V rms at 47–63 Hz and produces
+12 V, +5 V and −12 V, with a −5 V regulator on the logic board [1] p. 256. The
15-pin logic-board connector carries +12 V, four +5 V pins, six grounds, −12 V and
/PFW [1] Table 6-6 p. 251. Output limits for the II/IIx/IIfx supply: +5 V 4.90–5.20 V
at up to 18 A, +12 V 11.50–12.80 V at up to 2.5 A, −12 V −13.40 to −10.80 V at up to
1.0 A; 132 W maximum sustained, 156 W peak (15 s, 10% duty); ripple 20–50 mV
[1] pp. 263–264.

---

## 6. Expansion

### 6.1 The NuBus slots

The IIfx main unit is the wide (18.66 in) six-slot enclosure it shares with the
Macintosh II and IIx [1] p. 26. The six slots use Apple's implementation of NuBus
[1] p. 32, with slot IDs $9–$E and the address geography of §3.1: standard slot space
$Fsxx xxxx (16 MB per slot) and super slot space $ssxx xxxx (256 MB per slot)
[1] p. 138. All Macintoshes through the IIfx conform to the original NuBus standard,
ANSI/IEEE 1196-1987, with Apple's usual deviations: −5.2 V is not provided (the pins
are tied together), and the spec-reserved pins A2 and C2 — grounded on the II, IIx and
IIcx — are **open** on the IIfx [2] p. 29.

The NuBus interface hardware is the three-chip BIU30/BIU2/CGTO set [1] p. 32; the
controller part numbers are BIU2 = 344S0075 and BIU30 = 344S0074 [2] p. 31. The
existence of a distinct bridge is a consequence of the two-subsystem design: the
processor bus runs at 40 MHz while NuBus is a 10 MHz synchronous bus, so the bridge
decouples the two clock domains in a way the single-chip controllers of the 16 MHz
machines never needed.

Interrupt-wise, each slot's line runs to the OSS, which ORs the six into the slot
interrupt and individually records the number of the interrupting slot in a register;
software reads that register to find where the interrupt originated [1] p. 157. The
per-slot OSS sources are bits 0–5 ([pic.md](pic.md) §2.6). A declaration ROM in the
top of a slot's standard space identifies the card to the Slot Manager; the format,
sResources and the search algorithm are [declaration-rom.md](../../hardware/nubus/declaration-rom.md)
§1–§5. NuBus cards can master the bus (§5.2) and reach the main logic board through
the translations of Table 3-10 [1] p. 140.

### 6.2 The Macintosh IIfx 68030 processor-direct slot

The PDS is a **120-pin** expansion connector on the main logic board [1] p. 32, carrying
the microprocessor's bus signals, arbitration signals and power, so a card can take
control of the bus as a bus master and reach RAM, ROM and the I/O devices [3] pp. 317,
327, [2] p. 28. The physical envelope is large — 12.9 in × 4.0 in — and the card power
budget is **+5 V at 2 A, +12 V at 175 mA, −12 V at 100 mA** [2] p. 30, Figure 4-1; the
power budget for a PDS card is identical to that of the NuBus card it might replace
[3] p. 359. The full pin assignment and per-signal loading/drive limits are the
expansion specification's Figure 15-2, Table 15-4 and Table 15-5 [3] pp. 327–334; the
machine-specific signals are its Table 15-8 [3] p. 339. Signal loading is strict —
some pins drive a single 74LS input, some two — and the specification warns that all
signals needed by a card should be buffered at the connector [3] p. 327. The connector
carries the 68030 arbitration set (/CBREQ, /BGACK, /BR, /BG), the function codes, the
size bits, and the full 32-bit data and address buses with separate address and data
buses (no address latches needed, no byte swapping between the MC68030 and the
connector) [3] pp. 330–333, 355.

**Pseudoslot design — the normal case.** A card that grounds /SLOT.E and carries a
declaration ROM is treated by the Slot Manager firmware as a NuBus card: the card
occupies **slot $E**, the driver for the NuBus equivalent works unchanged, and all
interrupts on the PDS's **/IRQ6** line are fielded as slot $E interrupts [3] p. 355,
[3] p. 356. The card should *not* use the 68030's /IPL2–/IPL0 lines, for compatibility
with the machine's firmware [3] p. 356.

**Non-pseudoslot design.** A card that forgoes the Slot Manager occupies either the
slow slot space $6000 0000–$6FFF FFFF or the fast slot space $7000 0000–$7FFF FFFF,
must ship its own driver, and uses the PDS's second dedicated interrupt line, **/IRQ15**,
all of whose interrupts are fielded as nonslot-$E interrupts [3] p. 355, [3] p. 356.
The levels of both /IRQ6 and /IRQ15 are fully programmable in the OSS [3] p. 356 —
they are OSS sources like any other (§5.1), and the OSS register that holds each is not
established in the corpus (open question 7).

**Cycle termination and timing.** Outgoing (processor-to-card) cycles support dynamic
bus sizing — I/O ports of 8, 16 or 32 bits — terminated by /DSACK0, /DSACK1 or /STERM
[3] p. 355. Incoming cycles (card-mastered, to main memory) are 32-bit synchronous and
terminated **only** by /STERM; cycles from the card to I/O devices terminate by
/DSACK1//DSACK0, except to NuBus, where reads and aligned longword writes terminate by
/STERM [3] p. 355. The board-level 16 µs /AS watchdog then generates /BERR [3] p. 355
(§3.3). A card design must include an oscillator for its own timing — the
specification states it is impossible to route clock lines over the main logic board
to the connector on a machine of this speed [3] p. 327 note.

**Speed.** The PDS sits in the 20 MHz I/O subsystem (§2.1). The processor shifts down to
the card's 20 MHz clock for accesses in the pseudoslot spaces ($Exxx xxxx or $FExx
xxxx) and in slow space $6xxx xxxx; in fast space $7xxx xxxx the processor stays at
40 MHz against the card's 20 MHz clock, buying access speed at the price of harder
timing design [3] p. 357.

**Bus mastering and cache.** The PDS is the highest-priority bus master (§5.2). A
mastering card uses /DS when addressing on-board devices, must complete its DMA cycle
within the normal processor cycle time, and can pull four-longword bursts out of the
memory subsystem with /CBREQ (§2.4); a PDS bus master cannot read from a NuBus card
[3] p. 358.

### 6.3 Memory and ROM expansion

Beyond SIMM banks (§2.3), the machine's other expansion surface is the ROM SIMM socket,
which accepts modules up to 8 MB [2] pp. 19–20, [1] p. 486, and the NuBus slots, which
can hold up to 2 GB of memory-mapped card RAM [1] p. 486.

---

## 7. Boot sequence summary

The sequence below is the reset-to-OS path of the shipped ROM, as established by the
annotated disassembly [5], cross-checked against Apple's descriptions of the pieces
the ROM exercises. Addresses are in the ROM's $4080 0000-based alias.

### 7.1 Reset vectors and POST entry

At reset the 68030 fetches its SSP and PC from address $0; the FMC's ROM overlay maps
the first 512 KB of the address space to ROM (§3.4), so both come from the ROM image.
The reset SSP reads $4147DD77 — the image's checksum longword, doing double duty — and
the reset PC is **$4080002A**, a single jump into the POST entry at $4080008C [5].

The POST entry is eleven instructions [5]: set SR to $2700 (supervisor, all interrupts
masked); seed the CACR with $2000 (on-chip caches disabled) and read it back as a
capability test; if present, clear the 68030's TC to zero (translation off); execute
RESET to assert the bus reset line to the peripheral chips; and dispatch to the
long-form POST init at $40802E00 [5].

### 7.2 Machine detect

The POST init establishes a temporary RAM stack at $2600 (the overlay is still
asserted), points the VBR at a POST vector table inside the ROM, and installs a
deliberately minimal bus-error handler — three instructions that restore the stack
from A5 and jump to A6 — because during detection bus errors are *expected control
flow*, and each probe sets A6 to its own "fall back to next probe" address before
faulting [5].

The machine-ID detect then runs a ladder of probes against the per-machine entry table
at $408032C4 [5]:

1. **Mirror probes on VIA1.** A walking-bit write/read test of VIA1's IER register,
   performed through candidate mirror offsets. The $20000-mirror probe fails on the
   IIfx (its mirror period is $40000); the IIfx probe confirms its own $40000 mirror
   and back-proves the absence of the $20000 mirror that would indicate a IIx/IIcx/
   SE/30; a mirror-free probe path covers the IIci [5].
2. **The FMC bus-error trailer.** The IIfx probe data block directs the ROM to touch
   $50F2 6000 / $50F2 4000 — the undecoded window — and expects a bus error, which the
   minimal handler converts into "advance to the next probe" [5].
3. **Entry-table match.** The surviving candidate machine-ID byte for the IIfx is $06,
   matched against the entry record at $408037C0, which resolves (through a signed
   relocator) to the per-machine descriptor at $40803530 — the I/O base table of
   §3.2 [5].

The detect returns a feature bitmask that says which of the machine's devices are
present and must be initialized; for the IIfx it includes the VIA1, the OSS and the
IOPs [5]. Each set bit then drives one early initialization: VIA1 receives a six-byte
register init plus $7F to its IER (all interrupt enables off) via the descriptor's
$50F0 0000 base; the OSS receives the value $0D written to its ROM-control register at
$50F1 A204 ([pic.md](pic.md) §2.5); and the SCC IOP's bypass window receives its setup
pattern via $50F0 4020 [5].

### 7.3 The POST self-test phases

The secondary POST chain runs the machine's self-tests [5]:

| Phase | Address | What it tests |
|---|---|---|
| — | $4084114A onward | RAM test/zero chain; the machine's memory is pattern-filled and verified before use [5] |
| $8F | $40842E34 | the FMC's 16-bit serial shift register at $50F1 C000: sixteen single-bit writes then sixteen reads must round-trip the pattern, LSB first [5] |
| $90 | $40842EB2 | an FMC/ROM-mirror coherency test: with the OSS ROM-control register's bit 3 cleared or set, the ROM mirror page at $4000 8000 must read back normal or inverted respectively, against a copy patterned into low RAM; the phase toggles the bit four times and swaps the 68030 CACR between cache configurations while comparing [5] |
| $91 | $40842FC4 | the OSS register file: every level register round-trips values 0–6 under mask $07, and the ROM-control and counter-control registers round-trip under their own masks — [pic.md](pic.md) §4.2 |
| $92 | $4084306E | the OSS autovector path: for each level 6 down to 1, a three-instruction handler is installed at the level's autovector, the level is written to source 10's mask register (whose write pulses the source), and the handler must run — [pic.md](pic.md) §4.2 |
| $93 | $408430DC | RPU absence: 32 set/clear attempts against $50F1 E000 must *all* bus-error when the parity option is not installed; any completing access fails the test [5] |

Phase $93 is the parity-option probe: the OSS-extension window only answers when the
RPU is present, so the test's all-fault outcome both passes POST and leaves the
machine's configuration record without the parity feature [5]. The result is later
consumed by the OS-level parity and Gestalt plumbing ([iop.md](iop.md) §19).

### 7.4 From POST to the OS

After POST the ROM drops the ROM overlay (§3.4; open question 3), finishes the memory and low-memory
setup, and downloads the two IOPs' firmware — each an 'iopc' resource in the ROM, loaded
into the IOPs' shared RAM through the register window while the IOP is held in reset,
then released and confirmed alive ([iop.md](iop.md) §9). The SCSI manager arms the SCSI
DMA wrapper's interrupt enable ([scsi-dma.md](scsi-dma.md) §4.5). The Slot Manager then
scans the six NuBus slots for declaration ROMs — which is how this headless machine
finds its boot screen (§2.6) — and the boot-device search walks the drive queue for a
bootable volume [5]. The Start Manager finally sets up the OSS interrupt priority
levels and matching vectors [1] p. 119.

Observed guest behaviour at the far end of this path: an alternative operating system
booted on the machine reaches its graphical login through the ROM path above, and its
kernel programs the OSS level registers to its own assignment before driving the SCSI
DMA engine in chunked bus-master arms ([6]; the whole interrupt dispatch shape is
[pic.md](pic.md) §4.4, the driver sequences scsi-dma.md §4.5).

---

## 8. Open questions

1. **Toolbox-level machine identity.** The VIA1 CPU.ID code, the ROM candidate byte $06
   and the ROM identity words are established; the value the Toolbox's machine-
   identification interfaces report for the IIfx, and the low-memory flags the boot
   blocks test, are not in the evidence corpus for this page. The claimed existence of
   a machine-ID register at $5FFF FFFC on this machine is likewise unverified.
2. **The FMC's identity.** No Apple document in the corpus names the FMC's part number
   or its register surface. Its only attested software-visible behaviours are the
   overlay (§3.4), the cache (§2.4), the serial shift register at $50F1 C000 and the
   ROM-mirror invert flip-flop behind OSS ROM-control bit 3 (§7.3) [5] — the last two
   established only through POST disassembly, and the flip-flop's normal-mode purpose
   is unknown.
3. **What drops the ROM overlay.** On the two-VIA machines the Reset handler clears the
   overlay through a VIA1 signal [1] p. 136; the IIfx's VIA1 has no such bit [1]
   Table 4-7, and no corpus document names the write that switches the IIfx to the
   normal address map.
4. **VIA1 pin conflicts.** The Guide's IIfx table marks VIA1 DRA bits 7, 5, 3 and 0
   reserved and names CPU.ID the only used bits [1] Table 4-7; the internal overview's
   VIA-history table lists PA3 as a modem clock select, PA5 as the floppy SEL line and
   PA7 as the SCC Wait/Request for the IIfx [2] Table 13.1; and the floppy chapter's
   connector table labels the FDHD SEL pin "from VIA1" for machines including the IIfx
   [1] Table 9-7 p. 346, while the VIA-functions section says an IOP provides the SEL
   line on this machine [1] p. 155. Which source is right for each pin — and whether the
   IOP path and the VIA pin coexist — is unresolved.
5. **Clock generation.** The 80 MHz oscillator, the 40/20 MHz subsystem clocks and the
   IOP/SWIM clocks are established, but no corpus document names which chip generates
   the 15.6672 MHz C16M/IOCLK on this machine, the VIA's 783.36 kHz E clock, or the
   60.15 Hz /VBLK that the OSS monitors (on the II the GLUE generated the family's
   clocks; the IIfx's GLUE equivalent for these lines is unnamed).
6. **The NuBus bridge chips.** BIU2 and BIU30 have part numbers [2] p. 31; CGTO does
   not. The register semantics of the $50F1 8000 configuration window — write posting,
   read prefetch, any NuBus error recording beyond the /TM0A//TM1A convention of the
   two-VIA machines — are unpublished. The two OSS-extension windows at $50F1 C000 and
   $50F1 E000 are known only through POST behaviour (§7.3).
7. **The PDS interrupt lines in the OSS.** The expansion specification promises two
   dedicated OSS interrupt lines to the PDS, /IRQ6 (fielded as slot $E) and /IRQ15
   (fielded as nonslot $E) [3] p. 356, but the OSS source map carries no PDS row beyond
   the six NuBus slots [pic.md](pic.md) §2.6. Which OSS source numbers /IRQ6 and /IRQ15
   occupy — and how /IRQ6 relates to NuBus slot $E's own source — is not established.
8. **OSS internals carried from the device page.** The OSS's own open questions remain
   open at machine level too: the source numbering conflict, sources 12/13/15, the
   shutdown register's offset, the $204 bit semantics, the free-running counter, and
   the /SNDEXT bit's location ([pic.md](pic.md) §6).
9. **The undecoded-window owner.** The $50F2 4000–$50F2 7FFF bus error is
   ROM-observed [5], but whether the fault is raised by an FMC decode or by the OSS's
   general bus time-out [1] p. 119 — and the window's exact width — are not documented.
10. **A second SCSI register aperture.** All observed software drives the whole SCSI
    DMA chip at $50F0 8000, 53C80 cell at +$000 and DMA registers at +$080 [5], [6];
    whether the board additionally decodes a separate 5380-compatibility aperture at
    I/O offset $A000 (as the earlier machines do at their own offsets) is not
    established from primary evidence.
11. **NuBus interrupt latency.** The classic developer treatment of NuBus interrupt
    latency on the II-family machines is referenced by Apple's internal overview [2]
    p. 15, but no IIfx-specific latency figure (with its programmable OSS levels and
    the three-chip bridge) exists in the corpus.
12. **NuBus '90 features.** The IIfx conforms to the 1987 NuBus standard [2] p. 29, and
    the 1992 card book treats block transfers as a Quadra-family feature; whether the
    IIfx connectors carry the /CLK2X pair or any NuBus '90 signals is not documented in
    the corpus.
13. **The 8 MHz SCC claim.** The internal overview's machine table lists an 8 MHz 8530
    on the IIfx against 4 MHz parts on the IIx/IIcx [2] ch. 1 Table 1.4; no other
    corpus document confirms the SCC's clock input on this machine, and the F19 theory
    of operation's clock-source table for the IOP allows several RTXC sources [4] §1.2.1.

---

## References

1. Apple Computer, Inc., *Guide to the Macintosh Family Hardware*, second edition,
   Addison-Wesley, 1990. (IIfx feature list pp. 32–33; enclosure and slots p. 26;
   block diagram p. 59; data buses and speed-shift p. 61; interrupts p. 63; cache
   pp. 67, 228–231; IOPs pp. 109–110; general logic Table 3-7 p. 111; OSS p. 119;
   address-map discussion pp. 120–121; II-family map and NuBus translations
   pp. 135–140; IIfx address map pp. 141, 143; VIA functions pp. 154–157; VIA1 DRA
   Tables 4-7/4-8 pp. 164–165; SIMM rules pp. 209–216; IIfx SIMM signals Table 5-7
   pp. 214–215; parity pp. 222; ROM p. 232; power pp. 245, 251, 256–264; ADB p. 295;
   floppy Table 9-7 p. 346 and Figure 9-14 p. 355; serial Figure 10-6 p. 370; SCSI
   pp. 391–396; sound pp. 439–440; specifications pp. 486–487.)
2. Apple Computer, Inc., *Macintosh Hardware Overview*, revision 2, 11 February 1991
   (Apple internal engineering document). (Machine summary tables ch. 1, Tables 1.1–1.5
   pp. 8–9; interrupts pp. 14–15; cache p. 16; parity and ROM SIMMs pp. 19–20;
   expansion pp. 28–31; PIC pp. 32–33; SCSI DMA p. 34; power control p. 62; VIA
   history Table 13.1 pp. 63–64.)
3. Apple Computer, Inc., *Designing Cards and Drivers for the Macintosh Family*, third
   edition, Addison-Wesley, 1992. (68030 Direct Slot ch. 15 from p. 317; IIfx PDS
   electrical description pp. 327–334; machine-specific signals Table 15-8 p. 339;
   IIfx expansion card design — pseudoslot, termination, interrupts, bus-master
   priority, clock speeds, cache use, power — pp. 354–359.)
4. Apple Computer, Inc., *Peripheral Interface Controller Specification*, revision 1 —
   the Macintosh IIfx "F19" theory of operation. (PIC overview and host interface
   §1.0–§1.2; DPLL §1.2.1; SWIM, SCC and timing §1.3–§1.5; the theory-of-operation
   continuation covering the sound circuit, the SWIM clocking, the ADB connector and
   the soft-power circuit.)
5. Macintosh IIfx boot ROM (512 KB image dated 5 February 1990, checksum $4147DD77),
   annotated disassembly. (Reset vectors and POST entry $4080002A/$4080008C; POST init
   $40802E00; bus-error handler $40802E84; machine-ID detect $40802F18 with VIA1
   mirror probes $40803064/$40803082/$408030C2 and inner routine $40803122; entry
   table $408032C4 with IIfx record $408037C0; per-machine descriptor $40803530;
   POST phases $8F $40842E34, $90 $40842EB2, $91 $40842FC4, $92 $4084306E,
   $93 $408430DC; boot-device search $40801360.)
6. A/UX 3.0.1 kernel, annotated disassembly of the bootstrap and SCSI driver block
   (level-2 OSS dispatcher in `pstart`; chip-base setup to $50F0 8000; the chunked
   bus-master arming sequences). Observed guest operating-system behaviour; the same
   material anchors scsi-dma.md §4.5.
