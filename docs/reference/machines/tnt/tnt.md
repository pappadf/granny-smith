# The TNT family — Power Macintosh 7500/8500/9500

**Contents:**

1. [Overview & membership](#1-overview--membership) — the platform, its one ROM, the machines that run it, how
   this page splits against the machine and device pages
2. [Board architecture common to the family](#2-board-architecture-common-to-the-family) — the three buses, the
   processor card, the memory subsystem, the video subsystem, endianness, machine identity, the boot contract
3. [Memory map & address decode shared by the family](#3-memory-map--address-decode-shared-by-the-family) —
   physical map, the F-segment, PCI memory space, the ROM, the logical (MMU) map, the 68k decoder tables
4. [Device roster](#4-device-roster) — every chip on the board, its Apple part number, its device page
5. [Interrupt, bus, and clock architecture](#5-interrupt-bus-and-clock-architecture) — the single interrupt
   collector, the AR bus, PCI arbitration, the clock set, reset and power
6. [Per-machine index](#6-per-machine-index) — the 7500, 8500, 9500, the 7200 sibling, the 1997 follow-ons
7. [Open questions](#7-open-questions)

---

## 1. Overview & membership

### 1.1 What the platform is

The **TNT** platform is Apple's second-generation Power Macintosh logic board: the first Macintosh
platform built around the PCI expansion bus rather than NuBus, and the first whose hardware
initialisation is performed by **Open Firmware** rather than by a dedicated ROM bring-up blob. One
4 MB mask ROM serves the whole generation, and the ROM names the platform itself: its
configuration record carries `BootstrapVersion = "Boot TNT 0.1p"` [4], the string Apple's
NanoKernel builds for the 1995 Power Macintosh 7200, 7500, 8500 and 9500.

Apple's developer note introduces the generation by its deltas against the first-generation
(6100/7100/8100) machines, and the list is a complete statement of what the platform is:

- "a processor on a replaceable card for an easy upgrade to a more advanced microprocessor or
  coprocessor"
- "a memory system using 8-byte DIMMs and a 128-bit memory data bus for higher performance"
- "interfaces to I/O devices and expansion cards using the PCI expansion bus, an industry-standard
  bus with higher performance than the NuBus"
- "support for A/V features built into the main logic board" [1] §"Comparison With Earlier Models"
  p. 2

Where the first generation split the north side across a memory controller (HMC) and a separate
I/O controller (AMIC) and reached expansion cards through the BART NuBus bridge, TNT concentrates
the north in one custom part — **Hammerhead** (system bus, DRAM, ROM and L2 cache controller
[1] §"Hammerhead Memory Controller IC" p. 17) — bridges to PCI through one or two **Bandit**
ICs, and lands every legacy Macintosh peripheral on the **Grand Central** I/O controller, whose
descriptor-based DMA (DBDMA) architecture serves every I/O transfer on the machine [2]
§"Grand Central I/O System IC" p. 11. The 9500 developer note states the shape in one sentence:
"The architecture of the Power Macintosh 9500 computers is based on three buses: the processor bus
and two PCI buses. The processor bus connects the microprocessor and the memory; the PCI buses
connect the expansion slots and the I/O devices" [2] §"Main ICs and Subsystems" p. 8.

The platform is an "OldWorld" Macintosh in the community's terminology: the Mac OS Toolbox ROM
lives in hardware (a 4 MB, 100 ns ROM SIMM [1] §"Read-Only Memory" p. 16), the boot firmware is
Open Firmware 1.0.5 in the same image [4], and Mac OS is launched from ROM by a NanoKernel that
also hosts the 68k emulator. No operating system for these machines hard-codes hardware addresses;
every shipping system — Mac OS, Linux, NetBSD and MkLinux alike — reads them from the Open
Firmware device tree the ROM builds at startup [4], [8].

The subject of this page is what every board in the family shares. Register-level and
behaviour-level detail lives in the device pages (§4), and per-machine deltas belong to the
machine pages (§6); the three levels together are the specification of the platform.

### 1.2 Membership

The machines are identified by hardware registers, not by name plates: both halves of the ROM
read a Hammerhead identity register and a Grand Central board register and synthesize the model
identity from them (§2.6). The table below gives the membership with those identities; the
per-machine index (§6) carries the detail.

| Machine | Apple platform codename | Processor(s) as shipped | Gestalt ID | Bandits | Machine page |
|---|---|---|---|---|---|
| Power Macintosh 7200 | Catalyst | PowerPC 601, soldered | 108 | one | §6.4 |
| Power Macintosh 7500 | TNT | PowerPC 601 at 100 MHz, on a card | 68 | one | [pm7500.md](pm7500.md) / §6.1 |
| Power Macintosh 8500 | TNT | PowerPC 604 at 100 MHz, on a card | 69 | two | [pm8500.md](pm8500.md) / §6.2 |
| Power Macintosh 9500 | TNT | PowerPC 604 at 120 or 132 MHz, on a card | 67 | two | [pm9500.md](pm9500.md) / §6.3 |
| Power Macintosh 9500/MP | TNT | two PowerPC 604, on a card | 67 | two | §6.3 |
| Power Macintosh 7300 | TNT | 604-family, on a card | 109 | one | §6.5 |
| Power Macintosh 7600 / 8600 / 9600 (1997) | TNT | 604e / 604ev, on a card | as 7500/8500/9500 | as 7500/8500/9500 | §6.5 |
| Power Macintosh 9600/MP (1997) | TNT | two 604e | as 9500 | two | §6.5 |

Membership has two edges worth stating precisely. First, the **7200 is not a Hammerhead
machine**: it shares this ROM, this Grand Central, a Bandit and the whole I/O stack, but its
memory-and-video controller is **Platinum** (Apple part 343S1184), sitting at the same physical
base as Hammerhead with a completely different register file — see [hammerhead.md](hammerhead.md)
§1.2, and §6.4 below. The 1995 ROM's Open Firmware image does not even emit a `compatible` string
for it (§2.6). Second, the 1997 machines reuse the 1995 identities outright — the 8600 reports
itself to software as an 8500 and the 9600 as a 9500, because their ROMs carry a byte-identical
Open Firmware image with the same identity decode [5] — so the platform as the ROM defines it
already covered them (§6.5).

### 1.3 The one ROM

Every machine in §1.2 runs the same 4 MB mask ROM image, of which Apple shipped two 1995
releases (header checksums `$96CD923D` and `$9630C68B`) and two 1997 releases (`$960E4BE9`,
covering the 7300/7600/8600/9600, and `$960FC647`, a late-revision refresh of the 8600/9600) [4],
[5]. The image decomposes into six components [4]:

| Offset in image | Size | Component |
|---|---|---|
| +$000000 | 3 MB | `Mac68KROM` — the 68k Toolbox ROM, including the 68k emulator resources |
| +$300000 | 48 KB | PowerPC exception-table code — the reset entry lives at +$300100 |
| +$310000 | 64 KB | the NanoKernel (v01.01) |
| +$320000 | 64 KB | `HWInit` / POST — a diagnostics object, **not** the hardware bring-up (§2.7) |
| +$330000 | 128 KB | Open Firmware 1.0.5, a tokenized-Forth PEF bundle (100 444 bytes used) |
| +$360000 | 128 KB | the 68k emulator code, with its opcode table at +$380000 |

The two 1995 releases differ in exactly two components (`HWInit` and `Mac68KROM`); the
configuration record, NanoKernel, exception table and Open Firmware are byte-identical between
them [4]. The 1997 comparison is stronger still: of the six components, **Open Firmware is
byte-identical across all four images**, so the device tree — every address, `reg` property,
`AAPL,interrupts` number and Apple part number — is literally the same firmware on a 1997
machine as on a 1995 one, and only the page-table attributes (§3.5), the NanoKernel, the
exception table and six of the 180 Toolbox resources differ [5].

### 1.4 How to read this page set

The family split is: this page holds what every board shares; the device pages hold the chips;
the machine pages hold the deltas. Nothing in the device pages is restated here — they are cited
by section, e.g. [grand-central.md](grand-central.md) §3.2 for the interrupt source table,
[bandit.md](bandit.md) §2 for the PCI bridge register file,
[hammerhead.md](hammerhead.md) §3.2 for the DRAM organization,
[dbdma.md](dbdma.md) §3 for the DMA command set. A re-implementation of a machine reads this
page first, then the device pages it points at, then the machine page for the box in question.

## 2. Board architecture common to the family

### 2.1 The three buses

Every TNT board is built around one processor bus and one or two PCI buses.

The **processor bus** — the "AR bus", after the `Ar*` prefix its signals carry in Apple's
documentation of the derived Network Server design — is a modified, multiple-master version of
the PowerPC 60x system bus with "the addition of hooks for more than one caching agent" [3] §5.
It carries the processor (or, on the dual-processor configurations, both), Hammerhead as the slave
for RAM, ROM and the L2 cache, the Bandit bridge or bridges, and the Chaos display-bus bridge
[1] Chapter 2; [3] §4. Its data width is 64 bits [2] §"Data Interleaving" p. 10 — the memory
array behind it is wider still (§2.3). Bus timing, agent classes, coherency and arbitration are
covered in §5.2 and [hammerhead.md](hammerhead.md) §3.5–§3.6.

The **PCI buses** run at the industry-standard 33 MHz, asynchronously to the processor bus
[1] §"Bus Bridge" p. 17; [2] §"Bus Clock Rates" p. 11. A single Bandit (and thus a single PCI bus)
serves the 7500 and the 7200; the 8500 and 9500 carry two Bandits and two independent PCI buses
[2] §"Bandit Bus Bridge ICs" p. 11. The PCI slots hang directly off those buses, as does Grand
Central; the bridge itself, its configuration ports and its byte-order translation are
[bandit.md](bandit.md)'s subject.

The **video bus** is the platform's third bus, and the one the developer notes isolate
deliberately: "The video subsystem is connected to a separate bus" [1] §"Bus Bridge" p. 17 note.
The Control and Chaos ICs bridge it to the processor bus, and "the timing on the video bus is
synchronous with the main system bus" [1] §"Video Bus" p. 21 — the one bus bridge on the platform
that is not asynchronous. The on-board video data path is described in §2.4 and
[bandit.md](bandit.md) §3.10.

### 2.2 The processor card

"The main processor and its associated clock circuits are on a plug-in card. The processor card
provides an upgrade path to a faster or more powerful microprocessor" [1] §"Main Processor" p. 14;
[2] §"Processor Subsystem Card" p. 8. This one sentence drives two family-wide consequences.

First, **the bus clock is a property of the installed card, not of the board**. The card supplies
the system bus clock, which is why the documented rates differ per model — 50 MHz on the
7500/8500 [1] §"Bus Bridge" p. 17, 40 or 44 MHz ("one-third the processor speed") on the 9500
[2] §"Bus Clock Rates" p. 11 — and why the same logic board family spans 100-to-350 MHz
processors across the 1995–1997 generation (§6.5). The derived Network Server documentation
states the rule directly: "to enable processor daughtercard upgrades, a means is required by
which the motherboard acquires clocking from the processor card", with supported system speeds of
40–50 MHz [3] §2.10.

Second, **the ROM cannot assume which PowerPC it is running on**. The 7500 ships a 601, the
8500/9500 a 604 (16 KB separate instruction and data caches, four-way set associative
[1] §"PowerPC 604 Microprocessor" p. 16), and the whole upgrade-card ecosystem spans the 603,
604e, 604ev and 750 families. The 68k half of the ROM therefore runs a CPU-identification
dispatch at every boot, and the Open Firmware root node publishes the CPU identity it found as
the `AAPL,cpu-id` property, alongside the timebase, cache and TLB geometry the tree's `cpus`
node carries [4]. The multiprocessor configurations (9500/MP, 9600/MP) put two 604-class parts on
the one card; Hammerhead carries the small register set that makes that work — the arbiter
configuration, the who-am-I latch and the interprocessor interrupt — covered in
[hammerhead.md](hammerhead.md) §2.5–§2.7 and §4.5.

### 2.3 The memory subsystem

The memory subsystem is Hammerhead's: "A custom IC called Hammerhead controls the memory
subsystem. The components of the Hammerhead IC are the system bus controller, the DRAM
controller, the ROM controller, [and] the second-level (L2) cache controller" [1] §"Hammerhead
Memory Controller IC" p. 17; "The Hammerhead IC controls the memory and cache subsystem, which
includes the system bus, the main memory, the ROM, and the L2 cache" [2] p. 10.

Main memory is DRAM on 8-byte (64-bit) DIMMs. Two facts define the family contract:

- **The array is 128 bits wide where it can be.** "When the startup software detects two DIMMs
  that contain the same amount of memory, it configures their combined memory as a single bank
  with a memory data bus 128 bits wide" [1] §"Random-Access Memory" p. 16; on the 9500, "the
  memory controller in the Hammerhead IC can support data read operations 128 bits wide by
  interleaving the data from two DIMM slots", with the pair enabled for interleaving when the
  startup software finds same-size DIMMs in adjacent banks, and with critical-word-first ordering
  following the processor's preference (critical quadword for the 601, critical doubleword for
  the 603/604) [2] §"Data Interleaving" p. 10.
- **Physical memory is contiguous from $00000000.** "Memory addresses are contiguous, starting at
  address $0000 0000" [2] §"Random-Access Memory" p. 9 — unlike the discontiguous first-generation
  layout. Contiguity is manufactured by Hammerhead's bank base registers: "The Hammerhead IC
  contains a bank base register for each bank of main RAM... The control bits set the address
  multiplexing mode and enable or disable data bus interleaving", and "the base address for each
  bank is based on the sum of the sizes of all the lower numbered banks" [2] §"Bank Base
  Registers" p. 10. The register file that does this is [hammerhead.md](hammerhead.md) §2.10.

The controller ceiling is 1.5 GB in both notes ("supports main memory sizes up to 1.5 GB"
[1] p. 17; "The memory controller in the Hammerhead IC supports up to 1.5 GB of main memory"
[2] p. 9) against a 2 GB address allocation [2] p. 9; the per-machine slot counts (8 or 12) and
what they actually reach are in §6.

The second-level cache is an optional part of the subsystem everywhere except the 9500, which
solders 512 KB of it down [2] §"Second-Level Cache" p. 10. The cache is "a write-back cache; it is
direct mapped (single set) with allocate on read or write", with a synchronous-burst-SRAM data
store [1] §"Second-Level Cache" p. 17. The size is sensed, not configured: "The Hammerhead
memory controller IC interrogates two pins of this connector during system startup, to determine
the size of the memory on the SIMM. If no SIMM is installed, pull-up resistors on these pins
cause the Hammerhead IC to disable all external cache operations" [1] p. 17. The L2 controller's
register surface, size codes and POST bring-up are [hammerhead.md](hammerhead.md) §2.8–§2.9,
§4.4.

Refresh is CAS-before-RAS, one cycle every 15.6 µs [1] §"RAM Refresh" p. 43 — the same
architectural commitment as every earlier Macintosh generation, restated per generation because
it is a Hammerhead-programmed behaviour.

### 2.4 The video subsystem

The on-board video subsystem is TNT's second structural difference from Catalyst: where the 7200
folds the framebuffer into Platinum, the TNT machines keep a dedicated video bus with its own
bridge, framebuffer and RAMDAC. The subsystem "handles video input and output, mixes video with
computer graphics, and supports a wide variety of video monitors" [1] §"Video Subsystem" p. 20,
and is implemented by [1] §"Video Subsystem ICs" p. 20:

- **Chaos** (343S1155) — "a custom IC that provides data bus buffering between the video
  subsystem and the processor bus";
- **Control** (343S1154) — "a custom IC that provides addressing and control for the video
  subsystem";
- **RaDACal** — "a high-performance digital-to-analog converter (DAC)" for the monitor stream,
  which also generates the video timing and supports the hardware cursor;
- **Plan B** (343S1138) — the DBDMA controller for the video-input stream (§4.5);
- the **Sixty6** (343S1168) RGB-to-YUV convolver and a 7187 DENC for the second, television output
  stream — "on the Power Macintosh 8500 computer only";
- an **8758 ADC** and a **7196 DESC** for the video-input stream.

The framebuffer is four VRAM slots taking 1 MB SIMMs apiece; two SIMMs (2 MB) support up to
24 bpp on 17-inch monitors, four SIMMs (4 MB) up to 24 bpp on 21-inch monitors [1] §"Video Frame
Buffer" p. 20. The data path "through the video PCI to the VRAM is 64 bits wide", and "the output
data path from the VRAMs to the RaDACal high-performance DAC is 128 bits wide" [1] p. 20. With
the full 4 MB the frame buffer supports dual streams — the monitor plus the composite/S-video
output simultaneously, where 2 MB forces the main display off while the second stream runs
[1] pp. 20–22.

Video capture goes through the same DBDMA architecture as everything else: "Video data transfers
are DMA transfers and are controlled by a DBDMA engine in the Plan B IC", which "provides two
DBDMA channels for the 7196 DESC IC: a DBDMA write channel and a DBDMA read channel" — the write
channel moving pixels out of the DESC's FIFO into memory, the read channel fetching the
1-bit-per-pixel clip mask for play-through clipping [1] §"Video Input" p. 21. The engine
conventions are [dbdma.md](dbdma.md)'s; the video-bus side (Chaos's bridge behavior, Control's
register geometry) is [bandit.md](bandit.md) §3.10's.

### 2.5 Endianness — the one structural rule

The platform is big-endian at the top and little-endian at the bottom, and the seam is exactly
located: "Byte order for addressing on the processor bus is big-endian and byte order on the PCI
bus is little-endian. The Bandit IC performs the appropriate byte swapping and address
transformations to translate between the two addressing conventions" [1] §"Big-Endian and
Little-Endian Bus Addressing" p. 18; [2] p. 11. Everything behind Bandit — Grand Central's
registers and apertures, the DBDMA channel registers and in-memory descriptors, the machine
identity register, AWACS, the Bandit/Chaos configuration ports — is therefore reached by
PowerPC code through byte-reversing loads and stores (`lwbrx`/`stwbrx`), and everything north of
it — Hammerhead, RAM, ROM — is big-endian. The Bandit-side swapping and the address-invariant
conversion rules are covered in [bandit.md](bandit.md) §3.5 and
[grand-central.md](grand-central.md) §3.1; the family-level contract is just the seam itself.

A consequence worth stating at family level: the ROM's own PowerPC halves carry two register
disciplines in one image. The POST object reads Hammerhead with a plain big-endian `lwz` and the
board register with `lwbrx`, in adjacent routines [4]; an implementation that byte-swaps the wrong
side of the seam breaks the identity dispatch (§2.6) before anything else runs.

### 2.6 Machine identity

Two independent pieces of Apple software derive the machine model from hardware at every boot,
and their decodes agree — which is why identity belongs to the family page, with only the per-box
register values in §6. Both were recovered from the shipping ROM's code and verified live
(*observed* on the 7500, 8500 and 9500 profiles; details in
[hammerhead.md](hammerhead.md) §2.2–§2.4 and §4.3).

The **68k identification routine** reads three values: the Hammerhead identifier at `$F8000000`,
whose first byte `$39` selects the TNT personality (a `$3001xxxx` identifier instead selects
Catalyst — the 7200 branch); the machine-class flag at Hammerhead `+$20`, whose bit 30 marks the
9500; and the Grand Central **BoxID** register at `$F301A000`, whose bit 11 (read little-endian)
distinguishes the 8500 from the 7500. The synthesized product code is matched against the ROM's
product records, and the selected record carries the machine's gestalt value — `BoxFlag` reads
`$3E`, `$3F` and `$3D` on the 7500, 8500 and 9500 respectively, gestalt machine types 68, 69 and
67 (*observed*).

The **Open Firmware decode** reads the same two chips and selects the root node's `compatible`
string: Hammerhead `+$20`'s top byte selects the model class — bit 31 the 7500/8500 class, bit
30 the 9500 — and BoxID bit 13 (again little-endian) splits the 7500 class, set meaning 7500. A
recognized identity yields `compatible = "AAPL,7500"` (or 8500, 9500) followed by `MacRISC`; an
unrecognized one yields `"AAPL,????"` and **no display nodes** [4]. The full `compatible` list
the ROM can emit is `AAPL,7500`, `AAPL,8500`, `AAPL,9500`, `AAPL,7300`, `AAPL,????` — no
`AAPL,7200` exists anywhere in the 1995 or 1997 firmware, even though the 7200 boots this same
ROM and the Catalyst branch of the 68k decode identifies it by its product record
(*observed*; the absence is byte-established across all four ROM images [5], and where other
systems' `AAPL,7200` machine entries come from is §7.1).

### 2.7 The boot contract

TNT's cold-boot path is the structural inversion of the first generation: **Open Firmware
performs the hardware bring-up**. The `HWInit` component of the ROM is not hardware
initialisation on this platform — it is a diagnostics object (POST) whose symbol table names
`.POST`, `.L2DataCacheTest` and `.SerialTestManager` and whose entire hardware surface is the
NVRAM log port, the Hammerhead identifier and L2 registers, and the BoxID bit-8 factory-test
strap [4]. Everything else — probing the bridges and Grand Central, sizing memory, assigning
PCI BARs, building the device tree, formatting NVRAM — is Open Firmware's work.

The sequence, as established by the ROM's own code and configuration record [4]:

1. **Reset.** The processor fetches its hardware-reset vector at `$FFF00100` — inside the ROM
   decode, at image offset +$300100. The first instructions stash the exception state and branch
   into the NanoKernel.
2. **NanoKernel.** From the ROM's configuration record the kernel builds the initial BATs and
   page table (§3.5), finds the interrupt-controller address and the external-interrupt handler
   kind (`2` — the TNT handler, as against `1` for the first generation and `0` generic), and
   establishes the kernel, emulator and dispatch data pages.
3. **Open Firmware** (mapped at `$FF800000` logical, running from physical `$00400000`). It
   reads the identity registers (§2.6), sizes memory through Hammerhead, probes the Bandit
   bridges and Grand Central, builds the device tree, assigns the PCI devices' addresses, sets up
   the `aliases`, `options` and `chosen` nodes, and manages the NVRAM environment. On a blank or
   corrupted nonvolatile store it reformats the store and clears the Mac OS PRAM partition before
   the OS ever reads it (*observed* on cold boots from zeroed NVRAM [4]).
4. **POST** runs and logs its results into NVRAM through Grand Central's bank-select and data
   apertures [4]; [grand-central.md](grand-central.md) §2.6.
5. **`boot /AAPL,ROM`** — the default `boot-command`, present verbatim in the firmware [4] —
   loads the Mac OS boot path out of the ROM itself rather than an external device, and hands the
   PowerPC to the 68k emulator running the Toolbox half of the image. From there the classic
   Macintosh startup proceeds: low-memory setup, product/decoder selection (§3.6), driver
   loading, and the Start Manager's boot-device search.

Two family-level boot behaviours are worth recording because guests depend on them. The start-up
device search honours the PRAM default boot device first and only falls back to a full walk of
the drive queue after a spin-up timeout of roughly thirty guest-seconds — a machine with no disk
at the recorded ID therefore looks stalled for that long before it proceeds (*observed* on cold
boots with zeroed PRAM [4]). And the ROM's native video driver mode-sets the built-in display
through the Control registers early in the Mac OS start-up — the 9500, with no built-in
display, skips that path entirely and depends on the PCI card's FCode (§6.3) [4].

The firmware half of the contract is standardized: "The Open Firmware startup process in
PCI-compatible Macintosh computers conforms to the IEEE Standard 1275 for boot firmware and the
PCI Bus Binding to IEEE 1275-1994 specification" [1] §"Open Firmware Startup" p. 60; [6], [7].
Startup firmware is tokenized Forth (FCode); it builds "a data structure of nodes called a
device tree, in which each PCI device is described by a property list", stored in RAM for the
operating system to search [1] p. 60; expansion-card boot drivers are native PowerPC code
embedded in the card's FCode [1] p. 60. The tree this firmware builds is the platform's real
contract with every operating system — the canonical tree of a 9500 is published by Apple's
own device-tree technical note [8] — and its contents are enumerated per device in §4.

## 3. Memory map & address decode shared by the family

### 3.1 The physical map

The map below is the family's; every address in it is either published by the ROM's Open
Firmware device tree and 68k decoder tables [4], [8] or stated in the developer notes. The three
bridge slices, the Hammerhead window and the ROM decode are fixed by the firmware; the per-device
offsets inside the Grand Central window are enumerated in §4 and
[grand-central.md](grand-central.md) §2.5.

| Physical range | Size | Contents |
|---|---|---|
| $00000000 – RAM top | ≤ 1.5 GB | main DRAM, banked and made contiguous by Hammerhead (§2.3) |
| $00400000 | 4 MB | physical base of the Open Firmware working image |
| $80000000 – $8FFFFFFF | 256 MB | PCI memory space behind Bandit 1 |
| $90000000 – $9FFFFFFF | 256 MB | PCI memory space behind Chaos (the display bus) |
| $F0000000 – $F0FFFFFF | 16 MB | Chaos VCI bridge slice (config port at its base) |
| $F1000000 – $F1FFFFFF | 16 MB | Chaos-side device space: Control registers, RaDACal, the VRAM aperture |
| $F2000000 – $F2FFFFFF | 16 MB | Bandit 1 bridge slice, including its configuration ports |
| $F3000000 – $F3FFFFFF | 16 MB | Bandit 1's PCI I/O-side window — **Grand Central at its base** |
| $F4000000 – $F4FFFFFF | 16 MB | Bandit 2 bridge slice (8500/9500 only) |
| $F5000000 – $F5FFFFFF | 16 MB | Bandit 2's PCI I/O-side window |
| $F8000000 | ~2 KB | Hammerhead register window (Platinum on the 7200 — §6.4) |
| $FF800000 – $FFBFFFFF | 4 MB | Open Firmware, logically mapped (`LA_OpenFirmware`) |
| $FFC00000 – $FFFFFFFF | 4 MB | the boot ROM, including the reset vector at $FFF00100 |

Two structural facts about this map. First, the bridge and I/O placement is a published
allocation scheme, not an accident of wiring: the PCI book's allocation table assigns each
"PCI host bridge" a 32 MB control range — bridge 0 at $F0000000, bridge 1 at $F2000000, bridge 2
at $F4000000, bridge 3 at $F6000000–$F7FFFFFF, system control at $F8000000 — and leaves
$80000000–$EFFFFFFF and $F9000000–$FEFFFFFF "available to PCI expansion cards" [9] Table 2-1
p. 58. TNT populates bridges 0 (Chaos), 1 and 2 with the layout above; the bridge-3 range is
allocated by the scheme but unpopulated on every machine in this family (§7.8). Second, Grand
Central's base at $F3000000 "is a convention, not a decode constant of the chip" — the chip
"is fully PCI compliant and therefore can re-position its memory space response to anywhere in
PCI memory space", with $F3000000 fixed only by Apple's own firmware and Expansion Manager for
these ROM releases [3] §4.2.1 p. 12; [grand-central.md](grand-central.md) §1.3. Every shipping
piece of Apple software treats it as a constant anyway, including the 68k decoder tables [4].

### 3.2 Inside a bridge slice

Each Bandit owns one of the 16 MB control slices, and the coarse layout inside it is established
by the `ranges` property of a real 9500's bridge node [8] §"The PCI Bus" pp. 6–7 and detailed in
[bandit.md](bandit.md) §1.4 and §3.4:

| Region within Bandit 1's slice | Function |
|---|---|
| $F2000000 – $F27FFFFF (8 MB) | PCI I/O space window — PCI I/O address 0 at the bridge base |
| $F2800000 (4 bytes) | configuration address port |
| $F2C00000 (8 bytes) | configuration data port |
| $F2E00000 | special-cycle port |
| $F3000000 – $F3FFFFFF (16 MB) | PCI memory pass-through window: PCI addresses map 1:1 at the same physical addresses |

Bandit 2's slice mirrors Bandit 1's exactly at $F4000000 [4], [8]. The one-to-one pass-through
window is why Grand Central and every legacy cell have physical addresses inside the $F3 segment:
the 68k decoder table reaches them all as `$F3000000 + aperture` [4]. Chaos's slice is the same
idea for the display bus, with its own configuration ports and a device space at $F1000000 [4];
[bandit.md](bandit.md) §3.10.

### 3.3 PCI memory space

Below the F segment, PCI memory space opens at $80000000 behind Bandit 1 and at $90000000 behind
Chaos [4]; [bandit.md](bandit.md) §3.4 — the 256 MB each that the allocation table reserves
[9] Table 2-1 p. 58. Card frame buffers, Grand Central's repositionable response if firmware
ever moved it, and the on-board framebuffer aperture all live here; the frame buffer's use of
these apertures and the `assigned-addresses` mechanism that grants them are
[bandit.md](bandit.md) §4.2's subject.

### 3.4 The ROM

The ROM is a 4 MB, 100 ns SIMM decoded at $FFC00000 [1] §"Read-Only Memory" p. 16, and the
processor's hardware-reset vector $FFF00100 lies inside that decode — the first fetch of the
cold boot. Hammerhead is the part that serves it (§2.7 step 1; [hammerhead.md](hammerhead.md)
§3.1). The image layout is §1.3's table.

### 3.5 The logical (MMU) map

Before any operating system runs, the NanoKernel builds the initial translation from the ROM's
configuration record [4]. The family-relevant pieces:

| Mapping | Logical | Length | Physical | Attributes |
|---|---|---|---|---|
| ROM (and overlay) | $FFC00000 and $00000000 | 4 MB | ROM base | cache-inhibited (I), PP=RW |
| PowerPC code and kernel objects | $68000000 | 1 MB | ROM image + $300000 | the exception table, NanoKernel, OF bundle |
| Open Firmware | $FF800000 | 4 MB | $00400000 | — |
| Grand Central | $F3000000 | 1 page | identity | cache-inhibited |
| the whole I/O region | $F0000000 | 240 MB | identity | cache-inhibited |

Two family-level notes on this table. The **overlay**: the same BAT entry that maps the ROM at
$FFC00000 also maps it at logical zero for the earliest boot — ROM appears at physical 0 until
the kernel's own start-up replaces the mapping [4]. And the **guard bit**: the 1995 ROM maps the
I/O pages cache-inhibited but **not** guarded; the 1997 ROMs set the Guarded bit on every I/O
mapping (attribute $022 → $02A, $020 → $028) [5] — the difference between a 601-era and a
604e-era memory model — guarding blocks exactly the speculative, out-of-order access the later
processors perform (*inferred - unverified*; no Apple document states the reason for the
change). A guest that reads these attributes sees which generation of ROM it is
running.

The kernel-owned logical addresses are constants of the configuration record [4]:

| Symbol | Value |
|---|---|
| `LA_InterruptCtl` | $F3000000 — the interrupt controller, by name |
| `LA_KernelData` (KDP) | $68FFE000 |
| `LA_EmulatorData` (EDP) | $68FFF000 |
| `LA_DispatchTable` | $68080000 |
| `LA_EmulatorCode` | $68060000 — the 68k emulator |
| `LA_OpenFirmware` | $FF800000 |
| `PA_OpenFirmware` | $00400000 |
| `LA_HardwarePriv` | $FFF0C000 |
| `LA_InfoRecord` | $5FFFE000 |
| `InterruptHandlerKind` | 2 — the TNT external-interrupt handler |

### 3.6 The 68k decoder tables

The Toolbox half of the ROM does not read the device tree; it reads the classic decoder record —
the same decoder-record structure the earlier Macintosh ROMs carry, selected per product by the
identity dispatch (§2.6). The ROM carries two records, one per platform [4]:

| Decoder entry | TNT (7500/8500/9500) | Catalyst (7200) |
|---|---|---|
| ROM | $FFC00000 | $FFC00000 |
| GrandCentral | $F3000000 | $F3000000 |
| VIA1 | $F3016000 | $F3016000 |
| SCCRd / SCCWr (legacy aperture) | $F3012000 | $F3012000 |
| SCSI96_1 (internal) | $F3018000 — MESH | $F3010000 — the external 53C94 |
| SCSI96_2 (external) | $F3010000 | — |
| MACE | $F3011000 | $F3011000 |
| SWIM3 | $F3015000 | $F3015000 |
| AWACS | $F3014000 | $F3014000 |

Every base is inside Grand Central's window (§4), which is the point of the table: the 68k
Toolbox addresses the whole I/O subsystem through the pass-through window exactly as the native
half addresses it through the device tree [4]. Two traps are worth pinning at family level. The
record's field names are inherited from the 1994 layout, so the MESH slot is still called
`SCSI96_1` even though the silicon behind it is MESH, not a 53C96 — the register map must not be
inferred from the field name [4]. And the Catalyst record has **no** MESH entry at all, which is
the 68k half's expression of the 7200's missing fast-SCSI controller (§6.4) [4];
[grand-central.md](grand-central.md) §1.2. The SCC's second, ESCC aperture at +$13000 and the
legacy aperture's duality are [grand-central.md](grand-central.md)'s (§2.5).

## 4. Device roster

The roster below is the whole silicon set of a TNT board, with each part's Apple part number
(the ROM's own `model` strings [4]), its firmware node, and the page that owns its register
file and behaviour. Every part number in this table comes from the ROM's device tree
[4]; where a secondary source gives a different number, the ROM wins (§7.10).

| Device | Apple part | OF node / handle | Location | Device page |
|---|---|---|---|---|
| Hammerhead — memory/cache controller | 343S1142 | `hammerhead` | $F8000000 | [hammerhead.md](hammerhead.md) |
| Bandit 1 — PCI host bridge | 343S1126 | `bandit@F2000000`, alias `pci1` | $F2000000 | [bandit.md](bandit.md) |
| Bandit 2 — PCI host bridge | 343S1126 | `bandit@F4000000`, alias `pci2` | $F4000000 | [bandit.md](bandit.md) |
| Chaos — display-bus (VCI) bridge | 343S1155 | `chaos@F0000000`, alias `vci0` | $F0000000 | [bandit.md](bandit.md) §3.10 |
| Control — display controller | 343S1154 | `control` | Chaos device space, $F1000000 | [bandit.md](bandit.md) §3.10 |
| RaDACal — RAMDAC / video timing | — | in the Control node | Chaos device space | [bandit.md](bandit.md) §3.10 |
| Plan B — video-input DBDMA engine | 343S1138 | `planb`, `device_type "video-in"` | — | [dbdma.md](dbdma.md) §1.2 |
| Sixty6 — RGB-to-YUV convolver | 343S1168 | `sixty6` | Grand Central +$1C000 | — (AV, §6.2) |
| Grand Central — I/O controller, interrupt collector, DBDMA host | 343S1125 | `gc`, `device_type "dbdma"` | $F3000000 | [grand-central.md](grand-central.md) |
| Curio — MACE Ethernet + 53C94 SCSI + ESCC serial | — | `mace`, `53c94`, `escc` | Grand Central +$11000/+$10000/+$13000 | [grand-central.md](grand-central.md) §2.5 |
| MESH — fast internal SCSI | 343S1146 | `mesh`, alias `scsi-int` | Grand Central +$18000 | [grand-central.md](grand-central.md) §2.5 |
| AWACS — audio codec and control registers | — | `awacs` | Grand Central +$14000 | [grand-central.md](grand-central.md) §2.5; codec law: [AWACS](../pdm/awacs.md) |
| SWIM III — floppy controller | — | `swim3` | Grand Central +$15000 | [grand-central.md](grand-central.md) §2.5 |
| Cuda — microcontroller (ADB, PRAM, RTC, soft power) | — | `via-cuda` | Grand Central +$16000 (VIA1 transport) | [grand-central.md](grand-central.md) §2.5 |
| NVRAM — nonvolatile store | — | `nvram` | Grand Central +$1D000/+$1F000 | [grand-central.md](grand-central.md) §2.6 |
| 7196 DESC, 8758 ADC, 7187 DENC — AV video encode/decode | — | in the AV nodes | off Grand Central | §2.4 |

### 4.1 The north: Hammerhead

Hammerhead "controls the memory and cache subsystem, which includes the system bus, the main
memory, the ROM, and the L2 cache" [2] p. 10, and nothing south of the processor bus. Its
register window, DRAM organization, L2 controller and multiprocessor support are all
[hammerhead.md](hammerhead.md)'s; at family level the part matters here for the decode of §3.1,
the identity bit of §2.6, and the AR-bus slaveship of §5.2.

### 4.2 The bridges: Bandit and Chaos

Bandit "provides buffering and address translation between the processor bus and the PCI bus"
and "supports burst transfers, in both directions, of up to 32 bytes in length — the size of a
cache block" [1] §"Bandit PCI Bridge IC" p. 18. Chaos is the same architecture aimed at the
display bus instead of PCI, "cannot be probed for devices" the way Bandit can, and serves the
on-board video subsystem's dedicated bus (§2.1) [1] §"Video Bus" p. 21. Configuration-space
mechanics, the IDSEL map, the bridges' own device-11 headers, byte order and fault behaviour
are [bandit.md](bandit.md)'s.

### 4.3 The I/O controller: Grand Central and its cells

Grand Central "provides an interface between the standard Macintosh I/O devices and the PCI
bus" [1] §"Grand Central I/O Subsystem IC" p. 18. Its four family-defining functions are the
Cuda support (the VIA registers), "central system interrupt collection", descriptor-based DMA,
and the SWIM III floppy interface [1] p. 18; [2] p. 11. Its DMA controller "provides DBDMA
support for all I/O transfers, including transfers through its internal I/O controllers as well
as transfers through the Curio IC for other I/O devices" [2] p. 11 — the one sentence that
makes DBDMA the platform's single DMA architecture, with eleven channels on the TNT machines
(§4.4). Grand Central also "provides a 16-bit bus to several other devices, including the
nonvolatile RAM and the Sixty6 IC" [1] p. 18 — the generic bus its NVRAM, board-register and
AV apertures hang off.

The I/O cells it hosts [1] pp. 18–19; [2] pp. 11–13:

- **Curio** — "a multipurpose custom IC that contains a Media Access Controller for Ethernet
  (MACE), a SCSI controller, and a Serial Communications Controller (SCC)", with 8-byte FIFOs
  on both SCC transmit and receive streams, and DMA "between its I/O ports and the computer's
  main memory" [1] §"Curio I/O Controller IC" p. 19;
- **MESH** — the internal SCSI controller, faster than the external bus "because this bus does
  not have to drive a long external bus": up to 10 MB/s internal against 5 MB/s external [1]
  §"MESH High-Speed SCSI Interface" p. 19;
- **Cuda** — "a custom version of the Motorola MC68HC05 microcontroller" holding soft power,
  system reset management, parameter RAM maintenance, ADB control and the real-time clock [1]
  §"Cuda Microcontroller IC" p. 19;
- **AWACS** — "a custom IC that combines a waveform amplifier with a 16-bit digital sound
  encoder and decoder (codec)", conforming to the IT&T ASCO 2300 codec specification [1]
  §"AWAC Sound IC" p. 19;
- **SWIM III** — "an extension of the SWIM II design used in earlier Macintosh models", which
  "supports DMA data transfers and does not require disabling of interrupts during floppy disk
  accesses" [1] p. 18.

Every register-level fact about the cells and apertures — offsets, widths, the VIA1 window, the
interrupt block, the DBDMA channel file — is [grand-central.md](grand-central.md)'s.

### 4.4 The DMA architecture: DBDMA

Descriptor-based DMA is the platform's single I/O-transfer engine, standardised well beyond
this family: "DBDMA is a programming model used for MESH SCSI and ESCC today and potentially
other devices in the future" [10] §12.2 p. 147, with the Common Hardware Reference Platform I/O
device reference adopting the identical channel model for the legacy Apple devices [10]
Chapter 15. The engine, its command set, channel state machine and coherency rules are
[dbdma.md](dbdma.md)'s; the eleven-channel assignment, the channel-number-equals-interrupt-
number identity, and the device cells' use of the channels are [dbdma.md](dbdma.md) §1.4 and
§4.6.

### 4.5 The AV parts

The A/V capability that the first generation sold as a card is built into the TNT boards: video
input through the 8758/7196 pair with Plan B's two DBDMA channels, and — on the 8500 — a second
video output stream through the Sixty6 convolver and the 7187 DENC [1] pp. 21–22 (§2.4). The
DAV connector carries the same interface to PCI cards: "The computer has an internal DAV slot
for use by an optional digital video processor in a PCI expansion slot" [1] p. 6, and "A PCI
expansion card can transmit digital audio to the AWAC IC by way of the DAV connector" [1] p. 19.
The AV machines' per-machine wiring is §6.2's; the Sixty6 and Plan B register surfaces are the
least-documented parts of the platform (§7.9).

## 5. Interrupt, bus, and clock architecture

### 5.1 Interrupts: one collector

The platform has exactly one interrupt path into the processor, and it runs through Grand
Central. "Central system interrupt collection" is one of the part's four stated functions
[1] p. 18, and on TNT it is meant literally: Grand Central's own interrupt output is the only
path by which *any* device reaches the processor — including the on-board video controller,
which lives on a different bus (§2.1) but whose vertical-blanking interrupt is collected
externally like a PCI slot's line [4]. Thirty-two sources feed four registers; the interrupt
line has two acknowledge modes; the shipped kernel acknowledges, reads the level set and
classifies it into the emulated 68k interrupt priority levels; the VIA1 cascade carries the
Cuda's seven sources behind one bit. All of this — the source table, the mode semantics, the
kernel dispatch, the cascade — is [grand-central.md](grand-central.md) §2.2, §3.2–§3.5.

What belongs at family level is the shape:

| Property | Value | Where |
|---|---|---|
| Interrupt controller | Grand Central, at $F3000000 | [grand-central.md](grand-central.md) §2.2 |
| Source count | 32; a DBDMA channel's number is its interrupt number | [grand-central.md](grand-central.md) §3.2 |
| Delivery | one external interrupt into the PowerPC; the kernel classifies into 68k IPLs | [grand-central.md](grand-central.md) §3.4 |
| 68k IPL classes used | 1 (VIA1/Cuda), 2 (chips and external lines), 3 (Ethernet), 4 (all DBDMA channels and both SCC channels), 7 (the Cuda NMI) — no others | [grand-central.md](grand-central.md) §3.4 |
| Multiprocessor doorbell | the Hammerhead interprocessor interrupt, not Grand Central | [hammerhead.md](hammerhead.md) §2.7 |
| External lines | board wiring, not chip semantics — on-board video occupies a "PCI-slot-range" line on the TNT machines | [grand-central.md](grand-central.md) §3.2 |

The one behaviour to call out here because it is family-defining rather than chip detail: the
deassertion change. The shipped kernel stores the classified interrupt level into the emulated
environment and nothing between kernel re-entries lowers it; the only mechanism that lets it
fall is the interrupt-on-**change** behaviour of Grand Central's mode-1 latch, which re-enters
the kernel when an enabled source *drops*. Interrupt delivery therefore depends on line
transitions in both directions — the least obvious contract on the platform
(*observed*; [grand-central.md](grand-central.md) §3.3).

### 5.2 The processor bus

The AR bus is the 60x system bus with multiprocessor hooks [3] §5. Its family-level properties,
with the detail in [hammerhead.md](hammerhead.md) §3.5–§3.6:

- **Agents.** Masters, slaves, snooping masters, and a single arbiter [3] §5.1. On a TNT machine
  the bus carries the processor(s), Hammerhead (slave for RAM/ROM/L2), the Bandit(s) and Chaos.
- **Coherency.** "Coherence is maintained at a granularity of 32 bytes", and "level 2 caching
  agents force inclusion on the L1 of the 60x processors" [3] §5.1 — the L2 is inclusive by
  construction ([hammerhead.md](hammerhead.md) §3.4). There is no snooping across the PCI
  bridge: PCI masters' accesses are not coherent with processor caches, which is the
  coherency handshake's reason for existing ([bandit.md](bandit.md) §4.3).
- **Parity.** Data-path parity is checked, and parity errors generate a machine-check-class
  exception [3] §5.2.
- **Timing.** The bus runs synchronous to the logic-board ASICs, with the motherboard acquiring
  its clock from the processor card (§2.2); the AR-bus rate is therefore the installed card's
  system speed, 40–50 MHz across the family [3] §2.10.

### 5.3 PCI arbitration

PCI arbitration is external to Bandit: "A separate logic device (gate array) provides the
priorities for bus arbitration as follows: 1. Grand Central IC (I/O device controller; highest
priority) 2. PCI slots and Bandit master, in round-robin sequence: that is, each in turn, with
equal priority" [1] §"Bandit PCI Bridge IC" p. 18; [2] §"Bandit Bus Bridge ICs" p. 11. When a
DBDMA channel is running, it is Grand Central that masters the PCI bus on the device's behalf
[grand-central.md](grand-central.md) §1.3. The bridge's own view of arbitration, including its
holdoff register, is [bandit.md](bandit.md) §3.7.

### 5.4 Clocks

The platform's clock set, with each value's authority:

| Clock | Value | Authority |
|---|---|---|
| PCI bus | 33 MHz, published by the bridge node as `clock-frequency` | [1] p. 49; [2] p. 33; [8] p. 6 |
| Processor (AR) bus | 50 MHz (7500/8500); 40 or 44 MHz, one-third of the processor clock (9500) | [1] §"Bus Bridge" p. 17; [2] §"Bus Clock Rates" p. 11 |
| Video bus | synchronous with the main system bus | [1] §"Video Bus" p. 21 |
| DRAM refresh | CAS-before-RAS every 15.6 µs | [1] §"RAM Refresh" p. 43 |
| 53C94 (external SCSI) | 25 MHz, device-tree `clock-frequency` | [4] |
| MESH (internal SCSI) | 50 MHz, assumed by shipping drivers | [11] |
| SWIM III step timer | 1 MHz countdown | [11] |
| VIA1 timer 1 | the Mac OS `Ticks` driver; a 60.15 Hz tick is behaviorally exact on this platform | *observed* (§7.5) |
| AWACS sample clocks | the 44.1 kHz family (44100, 29400, 22050, 17640, 14700, 11025 Hz) by rate code | [grand-central.md](grand-central.md) §3.7 |
| Video dot clock | 3.9064 MHz reference, rate = 3.9064 MHz × 2^P × N / M | [11] |

Two clocks deserve family-level comment. The processor-bus rate is published per machine in the
device tree as `bus-frequency`, and because it comes from the removable card (§2.2) it is the one
clock that legitimately changes when the owner upgrades the processor. And the two SCSI cells
run at different rates — 25 MHz for the external 53C94, 50 MHz for MESH — so the two buses'
timing constants are not interchangeable [4], [11].

### 5.5 Reset and power

Reset authority lives in the Cuda: "management of system resets" and "program control of the
power supply (soft power)" are two of its five stated functions [1] p. 19. At the architecture
level the family contract has three parts: the power-on fetch at the hardware-reset vector
(§3.4); the Cuda's soft-power and restart services, reached only through the VIA1 transport and
its interrupt [grand-central.md](grand-central.md) §3.5; and the nonvolatile store, which
survives resets and power cycles by construction — it is where PRAM, the RTC and the POST log
live, and on a blank store Open Firmware reformats it before the OS reads it (§2.7) [4];
[grand-central.md](grand-central.md) §2.6. The reset behaviour of individual parts — Hammerhead's
reset state, Bandit's configuration defaults, Grand Central's power-on interrupt mode — is each
device page's.

## 6. Per-machine index

The machine pages carry each box's full delta set; this index states the deltas that define the
machine. Identity register values are given per the decodes of §2.6.

### 6.1 Power Macintosh 7500 — [pm7500.md](pm7500.md)

The compact-desktop machine, and the platform's baseline: the entry in the comparison table
against which the others are stated [1] Table 1-3 p. 5. As shipped it carries a PowerPC 601 at
100 MHz on the replaceable card, eight DIMM slots good for 1 GB, an optional L2 cache SIMM
(256 KB to 4 MB), and a single Bandit — so the one PCI bus serves the three PCI slots, the DAV
connector and Grand Central alike. Built-in video is the Control/Chaos subsystem with 2 MB of
VRAM (expandable to 4 MB), **with video input but no video output**: "Video input: Built-in,
24 bpp... Video output: None" [1] Table 1-3 p. 5. The machine's ROM identity: Hammerhead
`+$20` bit 31, BoxID bit 13 set, `compatible = "AAPL,7500"`, `BoxFlag = $3E`, gestalt 68. Standard
I/O: two GeoPort serial ports, ADB, stereo sound in and out, one external SCSI port, Ethernet on
AUI and 10BASE-T connectors [1] p. 6. Because it is the one-Bandit machine, its three PCI slots,
the DAV connector and the whole Grand Central I/O stack share a single bridge's PCI bandwidth —
the structural limit that distinguishes it from the two-bridge 8500 and 9500, and the subject
its machine page elaborates. (The on-board video subsystem does not join that contention: it
hangs off its own Chaos bridge, §2.1.)

### 6.2 Power Macintosh 8500 — [pm8500.md](pm8500.md)

The tower machine, and the family's full-AV configuration: video input *and* the second video
output stream (the Sixty6 convolver plus the 7187 DENC, mirror or independent image, dual-stream
with 4 MB of VRAM) [1] pp. 9, 22. As shipped: a PowerPC 604 at 100 MHz on the card, eight DIMM
slots (1 GB), a 256 KB L2 SIMM installed, **two** Bandits — the second bridge serving three of
the machine's PCI slots on its own bus — and 3.5-inch drive space for three devices [1] pp. 9–10.
ROM identity: Hammerhead `+$20` bit 31, BoxID bit 11 set, `compatible = "AAPL,8500"`,
`BoxFlag = $3F`, gestalt 69. Its AV wiring — Plan B's capture channels, the Sixty6 interrupt,
the DAV connector's signals — is the machine page's subject; the parts themselves are §4.5's.

### 6.3 Power Macintosh 9500 — [pm9500.md](pm9500.md)

The stretched tower, and the platform's maximum configuration: "twelve DIMM slots that can
provide up to 1.5 GB" [2] §"Random-Access Memory" p. 9, a built-in 512 KB L2 cache [2] p. 10,
and six PCI slots served by "two Bandit custom ICs, one for PCI slots 1–3 and the other for PCI
slots 4–6", with a 90 W slot power budget at both 5 V and 3.3 V [2] §"PCI Expansion Slots" p. 33.
The 9500 has **no built-in video**: "The Power Macintosh 9500 computers require an external
monitor and a video display card installed in a PCI expansion slot" — the 9500/120 ships
Apple's own accelerated PCI graphics card, whose "boot ROM conforms to the IEEE Standard 1275"
[2] §"Video Display Card" p. 13 — so the ROM correctly publishes no `chaos`/`control` nodes for
it (*observed*; §2.6). ROM identity: Hammerhead `+$20` bit 30, `compatible = "AAPL,9500"`,
`BoxFlag = $3D`, gestalt 67. The **9500/MP** puts two 604s on the one processor card; the
multiprocessor surface is Hammerhead's three registers ([hammerhead.md](hammerhead.md) §2.5–§2.7,
§4.5), not a separate platform; the 9500/180MP's own page,
[pm9500mp.md](pm9500mp.md), records the card's lifecycle as firmware and operating systems use
it.

### 6.4 Power Macintosh 7200 — the Catalyst sibling

The 7200 shares this page's ROM, Grand Central, one Bandit, the Curio, Cuda, AWACS, SWIM III and
the whole DBDMA and interrupt architecture — and is *not* a TNT board. Its processor is a
soldered 601 (no processor card), and its memory-and-video controller is **Platinum** (343S1184),
which occupies the same $F8000000 window as Hammerhead with an entirely different register file
and also owns the framebuffer aperture; there is no Chaos, no Control, and **no MESH** — the
machine's single SCSI bus is the external 53C94 [4]; [hammerhead.md](hammerhead.md) §1.2;
[grand-central.md](grand-central.md) §1.2. The ROM's identity machinery handles it through the
Catalyst arm: the 68k dispatch matches the swapped identifier halfword $3001 and selects the
Catalyst product record (gestalt 108), and the Open Firmware image — which has no `AAPL,7200`
string at all (§2.6) — takes its Catalyst path and instantiates the `platinum` display node
[4], [5]. Everything in §3, §4.3 and §5 applies to the 7200 verbatim; nothing in §2.3, §2.4 or
the Hammerhead sections does. Its per-machine deltas belong on a Catalyst page set of its own.

### 6.5 The 1997 follow-ons — 7300, 7600, 8600, 9600

The 1997 machines are the same platform wearing faster processors. The 7600, 8600 and 9600
replace the processor card with a 604e/604ev class part (up to 200–350 MHz, with the larger "Mach
5" L2 on the top-of-range cards), run the system bus at 50 MHz, and — because their ROMs carry
the byte-identical Open Firmware image — **report the 1995 identities**: the 8600 answers
`AAPL,8500`, the 9600 `AAPL,9500`, and software can only tell them apart by the CPU's PVR, the
L2 configuration or the AV hardware, never by the identity registers [5], [11]. The 7300 (which
replaced the 7200, 7600's chassis aside) drops video input. The 9600/200MP is a 9600 with a
dual-604e card — the same three Hammerhead MP registers as the 9500/MP, and a ROM that contains
no multiprocessing library at all: MP is entirely a system-software concern above the
nanokernel, so the platform needs nothing new [5].

The 1997 ROMs' component diff against 1995 is small and fully characterised: Open Firmware
byte-identical; the configuration record changed only in the I/O pages' Guarded attribute
(§3.5); the NanoKernel and exception table rebuilt (their changes are kernel-emulator contract
details, not hardware); POST rebuilt but with every hardware probe instruction-identical —
Hammerhead was **not** revised; and exactly six of the 180 Toolbox resources changed, three of
them the video drivers, consistent with new display timings rather than new silicon [5]. Notably,
the sound codec the 1997 firmware publishes is still **AWACS** — there is no `screamer`
`compatible` string in any image in evidence, so no driver takes a Screamer path on these
machines even if the board carries the register-compatible superset part [5].

The 7600, 8600 and 9600 have their own machine pages in this tree —
[pm7600.md](pm7600.md), [pm8600.md](pm8600.md) and [pm9600.md](pm9600.md)
(the last covering the 9600/200MP) — which carry this section's facts
forward; the 7300, a Catalyst-platform machine rather than a Hammerhead
one, remains covered by this section and §6.4.

## 7. Open questions

1. **Where `AAPL,7200` comes from.** No ROM image in evidence — 1995 or 1997 — contains the
   string, yet other systems' machine tables carry an `AAPL,7200` entry they match against the
   `compatible` property [11]. A ROM revision not in evidence, a machine shipping its own ROM,
   or a defensive entry never matched live: unresolved (§2.6, §6.4).
2. **The Apple ERS documents for Hammerhead, Bandit and Grand Central.** Apple's own Network
   Server notes name the "Grand Central ERS" as the register-level reference [3] §4.6.1 p. 13;
   none of the three was ever published. They would close the DRAM-control register gap that no
   ROM, driver or kernel in evidence has been able to.
3. **The unconsumed bits of the identity registers.** Beyond the bytes the two decodes read
   (§2.6), the rest of the Hammerhead identifier, the low bytes of `+$20` and the whole of
   `+$30` are unattested — only their consumption is known
   ([hammerhead.md](hammerhead.md) §2.2, §2.4 and the §6 open questions).
4. **BoxID's remaining bits.** Bit 8 is behaviourally a factory-test strap (set, the boot
   detours into the ROM's serial test monitor — *observed*), but no document states it; and the
   meaning of the register's other bits (slot power present, SCC modem-line images, strap
   pulls) is only community-attested, not established from Apple material or the ROM [4].
5. **The VIA timer input clock.** The Mac OS `Ticks` rate is behaviorally exact at the classic
   60.15 Hz on this platform (*observed*, §5.4), but what Grand Central actually feeds the VIA
   timer is not attested in any document in evidence, and the ROM's own `timebase-frequency`
   literal was never isolated in the tree.
6. **What consumes the POST log.** POST writes its results into NVRAM through Grand Central's
   apertures [4]; no known software ever reads them back.
7. **The Mac OS interrupt tree's per-device table.** The ROM's `InterruptTreeTNT` library builds
   the same five levels the kernel dispatch produces, but its pattern-compressed
   `interruptableDeviceTable` has not been decoded from the PEF [4].
8. **The bridge-3 allocation.** The PCI allocation table reserves $F6000000–$F7FFFFFF for a
   fourth host bridge [9] Table 2-1 p. 58; no machine in this family populates it, and no
   Apple document says why the reservation exists.
9. **The Sixty6 and Plan B register surfaces.** The AV machines' convolver and capture engine
   are the least-documented parts of the platform: their Apple part numbers and tree nodes are
   ROM-attested (§4) but neither register file is established from primary material.
10. **343S1142 versus 343S1190.** The ROM names Hammerhead `AAPL,343S1142` on every machine in
    this family [4]; secondary coverage of the Network Server gives 343S1190. Later revision,
    or a different board's part: not reconciled by any document in evidence
    ([hammerhead.md](hammerhead.md) §1.1, §6).
11. **The `$F1000000` window across the family.** Chaos-side device space on the TNT machines
    (§3.1) and Platinum's framebuffer aperture on the 7200 (§6.4) occupy the same 16 MB
    window; whether every machine decodes it the same way, and what a 9500 — with no Chaos —
    does with it, is not established.
12. **Where the ROM reads the bus clock from.** `bus-frequency` is published per machine, but
    whether the firmware derives it from a Hammerhead register, from the Cuda, or from a strap
    on the processor card is not attested anywhere in evidence.

## References

1. Apple Computer, Inc., *Developer Note: Power Macintosh 7500 and Power Macintosh 8500
   Computers*, Developer Press, 1995 — Chapter 1 "Introduction": §"Comparison With Earlier
   Models" p. 2 (the four headline features of the generation), Table 1-1 p. 3, Table 1-3
   p. 5 (per-model comparison: processors, L2, RAM slots, video in/out, expansion slots),
   §"Features of the Power Macintosh 7500" p. 6, §"Features of the Power Macintosh 8500" p. 9,
   §"NuBus Expansion Cards" p. 11; Chapter 2 "Architecture": §"Main Processor" p. 14 (the
   plug-in processor card), §"PowerPC 604 Microprocessor" p. 16, §"Read-Only Memory" p. 16,
   §"Random-Access Memory" p. 16, §"Second-Level Cache" p. 17, §"Hammerhead Memory Controller
   IC" p. 17, §"Bus Bridge" p. 17, §"Bandit PCI Bridge IC" p. 18, §"Big-Endian and
   Little-Endian Bus Addressing" p. 18, §"Grand Central I/O Subsystem IC" p. 18, §"Curio I/O
   Controller IC", §"Cuda Microcontroller IC", §"MESH High-Speed SCSI Interface", §"AWAC Sound
   IC" p. 19, §"Video Subsystem" and §"Video Subsystem ICs" p. 20, §"Video Frame Buffer" p. 20,
   §"Video Bus" p. 21, §"Video Input" p. 21, §"Second Stream Video Output" p. 22; Chapter 4:
   §"RAM Refresh" p. 43, §"PCI Expansion Slots" pp. 49–50 (three slots, PCI rev 2.0, 5 V
   signaling, 50 W); Chapter 5: §"Open Firmware Startup" p. 60.
2. Apple Computer, Inc., *Developer Note: Power Macintosh 9500 Computers* (Power Macintosh
   9500/120 and 9500/132), Developer Press, 1995 — Chapter 2 "Architecture": §"Main ICs and
   Subsystems" p. 8 (three buses), §"Processor Subsystem Card" p. 8, §"Read-Only Memory" p. 8,
   §"Random-Access Memory" p. 9, §"Second-Level Cache" p. 10, §"Hammerhead Memory Controller
   IC" p. 10, §"Data Interleaving" p. 10, §"Bank Base Registers" p. 10, §"Bandit Bus Bridge
   ICs" and §"Bus Clock Rates" p. 11, §"Big-Endian and Little-Endian Bus Addressing" p. 11,
   §"Grand Central I/O System IC" p. 11, §"Curio I/O Controller IC" and §"Cuda Microcontroller
   IC" p. 12, §"AWAC Sound IC" and §"MESH SCSI Controller IC" p. 13, §"Video Display Card"
   p. 13; Chapter 4: §"PCI Expansion Slots" p. 33 (six slots, one Bandit per three, 90 W).
3. Apple Computer, Inc., *Network Server Hardware Developer Notes* (Apple Network Server 500/700),
   c. 1996 — §2.2 pp. 5–6 (Grand Central as "non-critical I/O" and its 128 KB response
   convention), §2.4.2 cache memory, §2.10 "Low-Skew Clocking" (the motherboard acquires
   clocking from the processor card; 40–50 MHz system speeds), §3 Network Server initialization
   (reset vector fetch through Hammerhead), §4 "Network Server Address Map" and §4.2.1 "Grand
   Central Device Registers" p. 12 (re-positionable PCI memory response; `$F3000000` as
   convention), §4.6.1 p. 13 (the "Grand Central ERS" named as the unpublished register
   reference), §5 "Network Server System Bus" (the 60x-bus signal set, agent types, 32-byte
   coherence, L2-forced L1 inclusion), §5.2 "Parity Handling".
4. Power Macintosh 7200/7500/8500/9500 boot ROM, August 1995 — 4 MB mask ROM in two releases
   (header checksums `$96CD923D` and `$9630C68B`), comprising the 68k Toolbox ROM, the PowerPC
   exception table, the NanoKernel v01.01, the `HWInit`/POST diagnostics object (entry points
   `.POST`, `.L2DataCacheTest`, `.SerialTestManager`), the Open Firmware 1.0.5 image and the
   68k emulator with its opcode table. Evidence used: the configuration record
   (`BootstrapVersion "Boot TNT 0.1p"`, the BAT/PMDT templates, `LA_*` addresses,
   `InterruptHandlerKind = 2`, the pre-seeded low-memory values); the component offsets and
   sizes of §1.3; the reset entry at image offset +$300100 matching the NanoKernel reset code;
   the decoded Open Firmware token stream (the root-node `compatible` list and `AAPL,cpu-id`
   construction, the `aliases` strings `/chaos@F0000000`, `/bandit@F2000000`, `/bandit@F4000000`,
   the per-node `model` part numbers, `reg` and `AAPL,interrupts` properties, the 25 MHz
   `53c94` clock literal, the `boot-command` `/AAPL,ROM`); the disassembled 68k
   machine-identification routine and the two `DecoderInfo` tables (TNT and Catalyst, each
   preceded by the $F8000000 longword); the disassembled POST object (the Hammerhead halfword
   test, the `lwbrx` BoxID read and bit-8 test, the NVRAM logging stores); and boot-time
   observation of the identity outcomes, the blank-store NVRAM reformat, the Start Manager's
   PRAM-default-first boot-device search, and the ROM video driver's Control mode-set.
5. Power Macintosh 7300/7600/8600/9600 boot ROMs, February 1997 — two 4 MB images (header
   checksums `$960E4BE9` covering the 7300/7600/8600/9600 and `$960FC647`, the late-revision
   8600/9600 refresh). Evidence used: the full six-component comparison against the 1995 pair
   (Open Firmware byte-identical across all four images; the configuration record differing
   only in the I/O pages' Guarded attribute, $022→$02A and $020→$028; the identical
   `DecoderInfo` pair at the same file offsets; POST rebuilt with every hardware probe
   instruction-identical; six changed Toolbox resources — the Control, Platinum and Sixty6
   video drivers, `.DAVAudio`, the native SCSI Manager 4.3 component and the RAM-disk driver);
   the absence of any `screamer` or `AAPL,7200` string in any image; and the identity outcomes
   (the 8600 reporting `AAPL,8500`, the 9600 `AAPL,9500`).
6. Institute of Electrical and Electronics Engineers, *IEEE Standard for Boot (Initialization
   Configuration) Firmware: Core Requirements and Practices*, IEEE Std 1275-1994 — the
   firmware architecture, the FCode tokenized representation, and the device-tree requirement
   the Macintosh Open Firmware implementation conforms to.
7. Institute of Electrical and Electronics Engineers, *PCI Bus Binding to IEEE 1275-1994*
   (IEEE Std 1275 PCI bus binding, revision 2.1) — the specification the developer note names
   as the second half of the startup process's conformance claim.
8. Apple Computer, Inc., *Fundamentals of Open Firmware, Part II: The Device Tree*, Apple
   Technical Note TN1062, September 1996 — the device tree of a Power Macintosh 9500: the
   `dev /` listing with `/bandit@F2000000`, `/gc@10`, `/bandit@F4000000`,
   `/hammerhead@F8000000` (§"Viewing the Device Tree" p. 2), and the bandit nodes' full
   property sets — `reg`, `ranges`, `bus-range`, `clock-frequency 01FCA055`, `slot-names`,
   `AAPL,interrupts` (§"The PCI Bus" pp. 6–7).
9. Apple Computer, Inc., *Designing PCI Cards and Drivers for Power Macintosh Computers*,
   Revised Edition, Apple Technical Publications, 1999 — Table 2-1 p. 58 (the address
   allocations of four peer host bridges and the ranges available to PCI expansion cards);
   Part 1 "The PCI Bus" for the host-bridge operation and addressing-mode rules the Bandit
   implements.
10. Apple Computer, Inc., International Business Machines Corporation, Motorola, Inc.,
    *PowerPC Microprocessor Common Hardware Reference Platform: I/O Device Reference*, Version
    1.0, May 1996 — §12.2 p. 147 (DBDMA as the programming model for MESH SCSI and ESCC);
    Chapter 15 "Descriptor-Based DMA" pp. 171–194 (the channel and command model the family's
    engines implement).
11. The Linux PowerPC "powermac" platform sources, 1996 onward —
    `arch/powerpc/platforms/powermac/feature.c` (the `pmac_mb_defs[]` machine table with its
    `AAPL,7200`, `AAPL,7300`, `AAPL,7500`, `AAPL,8500`/`8600` and `AAPL,9500`/`9600` entries);
    `drivers/scsi/mesh.h` (the 50 MHz clock assumption); `drivers/block/swim3.c` (the 1 MHz
    step-timer countdown); `drivers/video/fbdev/controlfb.h` (the 3.9064 MHz dot-clock
    reference and the 2^P × N/M rate law).
