# Hammerhead — the TNT north bridge

**Contents:**

1. [Overview](#1-overview) — what the part is, which machines carry it, division of labor, bus
   topology, clocking
2. [Register file](#2-register-file) — the $F8000000 window: part identifier, machine flags, the
   multiprocessor registers, the L2 registers, the bank base register file
3. [Behaviour](#3-behaviour) — address decode, DRAM organization and multiplexing, the L2 cache,
   processor-bus arbitration and coherency, reset and exceptions
4. [Programming model](#4-programming-model) — cold boot order, memory sizing, machine-identity
   dispatch, L2 bring-up, multiprocessor bring-up, what operating systems touch
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 What the part is

**Hammerhead** is the custom memory-controller IC of the PCI Power Macintosh platform — Apple
internal codename TNT. Where the first-generation Power Macintosh machines split the memory
system across a controller (the HMC) and a separate I/O controller (the AMIC), Hammerhead
concentrates the entire north side of the machine in one part. The developer note enumerates its
components as:

- the **system bus controller**
- the **DRAM controller**
- the **ROM controller**
- the **second-level (L2) cache controller** [1] §"Hammerhead Memory Controller IC" p. 17

The 9500 developer note states the same scope from the other side: "The Hammerhead IC controls
the memory and cache subsystem, which includes the system bus, the main memory, the ROM, and the
L2 cache" [2] §"Hammerhead Memory Controller IC" p. 10. The glossary definition is the most
compact: "A custom IC that controls the memory and cache system in Power Macintosh 7500 and 8500
computers" [1] Glossary.

The chip's own firmware-visible name comes from the boot ROM's Open Firmware device tree, whose
`hammerhead` node carries `model = "AAPL,343S1142"` — Apple part 343S1142 [4]. Secondary sources
covering the Network Server sometimes give the part as **343S1190**; the shipping ROM of the
7500/8500/9500 names 343S1142, and the two numbers are best read as a later revision or a
Network-Server-specific part (*inferred*; neither document reconciles them — see §6.12).

Hammerhead is a processor-bus device, not a PCI device: the PowerPC POST code reads its
identification register with a plain big-endian `lwz`, with no byte reversal [4], and its
registers sit in the $F8 segment of the physical map rather than behind a bridge. This is the
opposite of the platform's I/O parts, which live behind the Bandit PCI bridge and are
little-endian (see [grand-central.md](grand-central.md) §4).

### 1.2 Machines that carry it

| Machine | Platform | Processor | Hammerhead | L2 cache | RAM slots |
|---|---|---|---|---|---|
| Power Macintosh 7500/100 | TNT | PowerPC 601 at 100 MHz | 343S1142 | optional SIMM, 256 KB–4 MB [1] p. 17 | 8 DIMM |
| Power Macintosh 8500/100 | TNT | PowerPC 604 at 100 MHz | 343S1142 | 256 KB SIMM installed [1] p. 17 | 8 DIMM |
| Power Macintosh 9500/120, /132 | TNT | PowerPC 604 at 120/132 MHz | 343S1142 | built-in 512 KB [2] p. 10 | 12 DIMM |
| Power Macintosh 9500/MP | TNT | two PowerPC 604 | 343S1142 | as 9500 | 12 DIMM |
| Power Macintosh 7300 | TNT | PowerPC 604e class | — | — | — |
| Power Macintosh 7600/8600/9600 (1997) | TNT | PowerPC 604e class | — | — | — |
| Apple Network Server 500/132, 700/150 | 9500 derivative | PowerPC 604 | secondary sources give 343S1190 | 8500-compatible cache DIMM [3] §6.2 | 8 DIMM [3] §2.4.1 |
| Power Macintosh 7200 | Catalyst | PowerPC 601 | **none** — Platinum 343S1184 occupies the same window [4] | — | — |

The 1995 boot ROM carries the `compatible` string `AAPL,7300` alongside `AAPL,7500`, `AAPL,8500`
and `AAPL,9500` [4], so the TNT platform as the ROM defines it already contemplated the 7300. The
1997 machines of the same platform reuse the older identities outright: the ROM's identity logic
reports `AAPL,8500` on the 8600 and `AAPL,9500` on the 9600, because the later ROMs carry a
byte-identical Open Firmware image with the same decode [4]. Whether the 1997 boards carry a
revised Hammerhead is not established by any document in evidence (§6.14).

The 7200 row is the trap. Catalyst shares the TNT ROM, the Grand Central I/O controller and a
Bandit, but its memory-and-video controller is **Platinum** (343S1184), not Hammerhead, and it
sits at the *same* physical base, $F8000000 [4]. Nothing about Hammerhead's register offsets may
be ported to the 7200, and vice versa (§3.1, §6.1).

### 1.3 Division of labor

Hammerhead owns everything north of the PCI bridge and nothing south of it. The split across the
platform, established by the developer notes' component chapters [1] pp. 16–19, [2] pp. 8–13 and
the ROM's device tree [4]:

| Function | Owner |
|---|---|
| DRAM array control, bank base registers, refresh, interleaving | Hammerhead [1] pp. 16–17, 41–43; [2] pp. 9–10 |
| ROM SIMM control, reset-vector fetch at $FFF00100 | Hammerhead (ROM controller) [3] §3 |
| L2 cache controller, cache-SIMM size sensing | Hammerhead [1] pp. 17, 45 |
| Processor-bus (AR-bus) slaveship for RAM/ROM/L2 | Hammerhead |
| PCI host bridging, big/little-endian byte swapping | Bandit — see [bandit.md](bandit.md) [1] p. 18 |
| Display-bus (VCI) bridging | Chaos [1] §"Video Subsystem" p. 20 |
| I/O devices, DBDMA, interrupt collection | Grand Central — see [grand-central.md](grand-central.md), [dbdma.md](dbdma.md) [1] pp. 18–19 |
| Machine-identity register (BoxID) | Grand Central, at $F301A000 [4] |
| NVRAM, PRAM, RTC, ADB | Cuda, behind Grand Central [1] p. 19 |

The platform's interrupt system in particular is entirely Grand Central's: Hammerhead's only
documented paths into the processor's exception machinery are the parity error (§3.7) and the
inter-processor interrupt (§2.7).

### 1.4 Bus topology

The processor bus — which the Network Server hardware notes call the **ARBus**, after the `Ar*`
prefix of its signals [3] §4.1 — is a modified, multiple-master version of the 60x system bus,
"essentially identical to the 9500 system bus, with the addition of hooks for more than one
caching agent" [3] §5. The signal set is essentially that of the PowerPC 604 bus [3] §5.1, and the
L2 cache SIMM's data pins are labelled `ArDat(63:0)`, "processor data bus" [1] Table 4-7 p. 49 —
the cache sits directly on the 64-bit AR-bus data lines.

On a TNT machine the bus carries the processor (or processors, on the dual-604 configuration),
Hammerhead as the slave for RAM, ROM and the L2 cache, the two Bandit bridges (one on the 7500;
two on the 8500/9500 [4]), and the Chaos display-bus bridge. Everything else — Grand Central,
Curio, MESH, AWACS, Cuda — hangs behind Bandit 1 on the PCI bus [1] pp. 17–19.

There are four kinds of agent on this bus: masters (M), slaves (S), snooping masters (sM), and a
single arbiter (A) [3] §5.1. Coherence is maintained at a granularity of 32 bytes, and "level 2
caching agents force inclusion on the L1 of the 60x processors" [3] §5.1 — the Hammerhead L2 is
an inclusive cache by construction (§3.4).

### 1.5 Clocking

The processor bus and the PCI bus run asynchronously: the PCI bus at 33 MHz, the processor bus
at 50 MHz on the 7500/8500 [1] §"Bus Bridge" p. 17, and at 40 MHz or 44 MHz — one third of the
processor clock — on the 9500 [2] §"Bus Clock Rates" p. 11. The Network Server documents the
general rule: the system bus runs synchronous to the logic-board ASICs, and "to enable processor
daughtercard upgrades, a means is required by which the motherboard acquires clocking from
the processor card"; supported system speeds there are 40–50 MHz [3] §2.10. Because the processor
card supplies the clock, the bus rate is a property of the installed card, not of the board.

The same note records the electrical constraints that follow: the card's `refClk` must be a
fast-transition signal (minimum 1 V/ns, "F family or better"); the returned `clkToProc` is
asynchronous to `refClk` but synchronous to the system ASICs, running in advance of them by
approximately 700 ps; and processors with 0 ns output hold time — which includes the 604 —
require an inserted clock delay of at least 1 ns [3] §6.1.

## 2. Register file

### 2.1 The register window

Hammerhead's registers live at physical base **$F8000000**. The base is attested three ways: a
longword $F8000000 precedes both 68k `DecoderInfo` tables in the ROM, recording the north-bridge
base for the 68k half of the system; the Open Firmware literals for the `l2-cache`, `chosen` and
`bandit` nodes cite $F80000E0, $F80000C0, $F8000090 and $F80000F0 outright; and MkLinux's
multiprocessor header defines `HammerHead 0xF8000000` [4], [5].

The window's *size* is not stated for Hammerhead by any document. The best available figure comes
from the Catalyst sibling: the ROM's `platinum` node publishes `reg = F8000000 00000800` — a
2 KB register set at the same base [4] — and the NetBSD Platinum driver records 128 registers,
32-bit, on $10 centres (`PLATINUM_REG_OFFSET_ADDR(x) = x * 0x10`, `PLATINUM_REG_COUNT = 128`),
which is exactly 128 × $10 = $800 [6]. Hammerhead's four byte-wide registers at $90, $C0, $E0 and
$F0 sit precisely on those centres (indices 9, 12, 14 and 15), so the same 128-register,
$10-centred layout is the consistent reading for Hammerhead as well (*inferred — unverified*).
The register file is big-endian like the rest of the processor bus [4].

The attested register set is small, and it is the complete set the entire evidence corpus
attributes:

| Offset | Width | Attested by | Function |
|---|---|---|---|
| +$00 | 32-bit | POST code, 68k identification, Open Firmware model decode [4] | part/family identifier (§2.2) |
| +$20 | 32-bit | 68k identification, Open Firmware model decode [4] | machine-class flag (§2.3) |
| +$30 | 32-bit | Open Firmware model decode [4] | model identity, folded into `AAPL,cpu-id` (§2.4) |
| +$90 | byte | Open Firmware `/chosen` node; MkLinux `MPPlugIn.h` [4], [5] | `ArbConfig` — multiprocessor configuration (§2.5) |
| +$B0 | byte | MkLinux `MPPlugIn.h` [5] | `WhoAmI` — processor identity (§2.6) |
| +$C0 | byte | Open Firmware `/chosen` node; MkLinux `MPPlugIn.h` [4], [5] | `IntReg` — inter-processor interrupt (§2.7) |
| +$E0 | byte | Open Firmware `l2-cache` node; POST `.L2DataCacheTest` [4] | L2 presence, size and mode (§2.8) |
| +$F0 | byte | POST `.L2DataCacheTest`; the `bandit` node's literal pool [4] | L2 flush/fill strobe (§2.9) |
| +$1C0–+$4F0 | — | observed during firmware memory sizing (§2.10) | bank base register file |

Nothing in any document attributes the remaining offsets. The bank base registers that the 9500
developer note proves to exist (§2.10) are the one large block known to live in the file.

### 2.2 Part identifier (+$00)

A longword register that doubles as the machine-family discriminator. Three independent consumers
read it, at three widths:

1. **The POST diagnostics object** loads the longword big-endian, shifts right 16 bits, and
   compares the resulting halfword against **$3001**, returning a boolean in a register [4]:

   ```asm
   lis   r3,0xF800
   lwz   r3,0(r3)        ; read 0xF8000000, big-endian
   srwi  r3,r3,16
   cmpwi r3,0x3001       ; upper halfword == 0x3001 ?
   li    r28,1           ; boolean result
   ```

   The test is a *boolean*, and the constant is a **Catalyst** value: a machine whose identifier
   reads $3001xxxx takes the 7200 personality — the Catalyst `DecoderInfo` table, the
   `platinum` display path, and no MESH SCSI [4]. It is not a "TNT required value".

2. **The 68k machine-identification routine** (at ROM $FFC14844) reads only the *first byte*
   with `CMPI.B #$39,$F8000000`: byte 0 = **$39** selects the TNT family, and the routine
   distinguishes the individual TNT models from +$20 and the Grand Central BoxID register
   [4] (§4.3). The Catalyst arm of the same dispatch compares the swapped halfword against
   $3001, matching the POST test [4].

3. **Open Firmware's model decode** switches on byte 0 of the same register — $39 for the TNT
   family, $30 for the Catalyst path — before building the root node's `compatible` property
   [4] (§4.3).

Only the leading byte (on the TNT arm) and the leading halfword (on the Catalyst arm) are
consumed; the remaining bytes of the identifier are unattested by any source (§6.9).

### 2.3 Machine-class flag (+$20)

A longword whose top byte carries the machine class inside the TNT family. Two consumers:

- The 68k identification routine executes `AND.L #$40000000,$F8000020`: **bit 30 set selects the
  9500** [4].
- Open Firmware computes a model selector from the top byte of the register as
  `m = (byte0 >> 5) | ((byte0 >> 1) & 8)`: **$80 (bit 31) reads as m=4, the 7500/8500 class;
  $40 (bit 30) reads as m=2, the 9500**; anything else falls to an unidentified machine whose
  root node gets `compatible = "AAPL,????"` and whose tree is built with **no display nodes at
  all** [4].

The decode's consequences are far-reaching: the same Open Firmware image that boots a 7500 will
refuse to instantiate the `chaos` and `control` display nodes on a machine whose +$20 does not
decode to a known class (*observed*: a register answering $80000000 yields the 7500/8500 class
with `chaos`/`control` nodes present and Control's base addresses probed and assigned; $40000000
yields the 9500, which correctly gets no onboard-video nodes, matching the real 9500's lack of
built-in video) [4]. The low bytes of the register are unattested (§6.9).

### 2.4 Model identity (+$30)

The Open Firmware model decode folds the **top byte** of this register into the published
`AAPL,cpu-id` property, shifted right four bits into the value's low nibble [4]. No document
states what the register holds on a real board, and no other consumer of it is known; only its
consumption is attested (§6.8).

### 2.5 `ArbConfig` (+$90)

A byte register named outright by MkLinux's multiprocessor plug-in header [5]:

```c
#define HammerHead   0xF8000000
#define ArbConfig    0x0090      /* TwoCPU  0x02                    */
#define WhoAmI       0x00B0      /* PriCPU  0x10   SecCPU  0x08     */
#define IntReg       0x00C0      /* SecInt  0x80                    */
```

The header's own comment gives one known value: **$02 = `TwoCPU`** — a second processor is
present. Open Firmware cites the same address inside the construction of the `/chosen` node [4],
which is where a boot-time CPU count belongs. Whether the register is read-only status, a
configuration latch, or both is not established by the surviving uses (§6.10).

### 2.6 `WhoAmI` (+$B0)

A byte register, named by MkLinux with two values: **$10 = `PriCPU`** (this processor is the
primary) and **$08 = `SecCPU`** (secondary) [5]. It answers the question the dual-processor boot
flow depends on — "which processor am I?" — for code that must decide between continuing machine
initialization and entering the secondary's spin-wait (§4.5). Whether a uniprocessor machine
answers $10 is not stated by any source; the natural reading is that it does (*inferred*).

### 2.7 `IntReg` (+$C0)

A byte register — MkLinux reads it as `*(unsigned char *)(hammerh + IntReg)` [5] — cited by the
firmware's `/chosen` node construction alongside +$90 [4]. The header pairs the constant $80
with the name `SecInt`: **signalling the secondary processor**. The write direction is the
natural reading of the name and of the Network Server's boot description — the secondary comes
out of reset and "will enter a spin-wait for an interprocessor interrupt" [3] §3 — but no
surviving code shows the write; only the definition and one debug-mode read are attested
(*inferred* as to direction and semantics).

The reverse direction — secondary signalling the primary — does *not* go through Hammerhead on
the Network Server: `SecToPri_Int` arrives on Grand Central external interrupt 10 and is raised
by accessing the Ethernet PROM's chip-select space on the board's GBUS, at offset $19000 [3]
§4.5. There is no dedicated reverse-IPI register in Hammerhead's attested set.

### 2.8 L2 configuration and status (+$E0)

A byte register, fully decoded by the POST diagnostics object's `.L2DataCacheTest` routine [4]:

| Bits | Meaning |
|---|---|
| $80 | **L2 cache present.** If clear on the first read, the entire L2 test is skipped. |
| $07 | **L2 size code**, valid on a read taken after writing 0 to the register: `0` = 512 KB, `1` = 256 KB, `2` = 1 MB, `3` = 4 MB; any other value fails the power-on test. |
| $70 | written as a unit ($70) at the end of the test — presumably enable/mode bits (*inferred*; no source names them, §6.7). |

The presence bit and the size code are the two sides of the cache-SIMM sensing mechanism the
developer note describes in purely electrical terms: "The Hammerhead memory controller IC
interrogates two pins of this connector during system startup, to determine the size of the
memory on the SIMM. If no SIMM is installed, pull-up resistors on these pins cause the
Hammerhead IC to disable all external cache operations" [1] p. 17. The SIMM connector carries
`CacheSize(1:0)` — "cache size lines: b00 for 512 KB, b01 for 256 KB, b10 for 1 MB, b11 for 4
MB" — and a separate `CachePrsnt`, "active (low) if an L2 cache card is installed" [1] Table 4-7
p. 49. The four defined size codes match the POST's four decoded values exactly, and the pulled-
up state of the presence pin matches the cleared $80 bit: two independent sources describing one
mechanism [1] p. 17, [4].

### 2.9 L2 flush/fill strobe (+$F0)

A byte register used as a strobe. The POST sequence writes $80 to it, then walks a memory range
the size of the cache, then writes 0 [4] — assert, touch, deassert: a cache-fill/flush window.
The same address appears in the Open Firmware literal pool that builds the `bandit` node, among
the PCI bridge's address-allocation code [4] — a context the L2-strobe reading does not
obviously explain (§6.6).

### 2.10 The bank base register file

The 9500 developer note is the one Apple document that names Hammerhead's DRAM control
registers:

> "The Hammerhead IC contains a bank base register for each bank of main RAM. Each bank base
> register has space for a **base address and control bits**. The control bits set the address
> multiplexing mode and enable or disable data bus interleaving." [2] §"Bank Base Registers"
> p. 10

The bank base addresses are the mechanism that makes physical RAM contiguous: "The system
software calculates the base addresses based on the amount of memory in each of the SIMMs. The
base address for each bank is based on the sum of the sizes of all the lower numbered banks"
[2] p. 10. The 7500/8500 note confirms the software side: "The system software initializes the
address mode bits in the bank base registers as part of the process of determining the amount
of RAM installed in the computer" [1] §"RAM Address Multiplexing" p. 41.

No document gives the register file's offsets or field layout. The firmware pins its location
behaviourally: once the machine-identity reads answer as a TNT machine, Open Firmware's
memory-sizing pass writes and then reads back a contiguous register file spanning **+$1C0
through +$4F0** (*observed* against the shipped firmware [4]). The stride and the field packing
inside that range are not attested (§6.2) — the two attested control fields (address-multiplexing
mode, interleave enable) and the base address are all any source names.

### 2.11 Reset state

The power-on values of every register in the file are unknown; no source documents them, and no
shipped software reads back +$20, +$30, +$90, +$B0 or the bank file except immediately after
writing it. The firmware rewrites the bank file and the L2 registers on every cold boot, so
nothing in the observed software contract constrains their reset state (§6.1).

## 3. Behaviour

### 3.1 Address decode

Hammerhead answers three regions of the physical map: main DRAM, the ROM, and its own register
window. The 9500 note fixes the DRAM region's shape: "The address map for the Power Macintosh
9500 computers has 2 GB allocated for main memory. The memory controller in the Hammerhead IC
supports up to 1.5 GB of main memory. Memory addresses are contiguous, starting at address
$0000 0000" [2] §"Random-Access Memory" p. 9 — contiguity being the bank base registers' work
(§2.10).

The ROM is a 4 MB SIMM with 100 ns access time [1] §"Read-Only Memory" p. 16, [2] p. 8, decoded
at $FFC00000 [4]. The processor's hardware-reset vector, $FFF00100, lies inside that region, and
Hammerhead is the part that makes the fetch possible: "A processor comes out of hardware reset
and fetches the reset vector FFF00100. Hammerhead maps this to ROM space and non-cacheable
accesses to the ROM begin" [3] §3. The first code then distinguishes primary from secondary
processor and either proceeds with initialization or spin-waits (§4.5).

Its own registers decode at $F8000000. The rest of the $F0–$F8 segment belongs to the siblings —
Chaos at $F0000000 (with its device space at $F1000000), Bandit 1 at $F2000000 (with Grand
Central at the base of its PCI I/O window, $F3000000), Bandit 2 at $F4000000 — and each of those
decodes its own window, programmably in Bandit's case (see [bandit.md](bandit.md) §4.1) [4].
Below the segment, PCI memory space opens at $80000000 behind Bandit and at $90000000 behind
Chaos [4].

The practical consequence of the 2 GB/1.5 GB pair of statements is worth stating plainly: the
*controller* ceiling is 1.5 GB, and even that is reachable only on the 9500, whose twelve slots
of two-bank 128 MB DIMMs sum to exactly 1.5 GB; the 7500/8500 note states the same controller
ceiling ("supports main memory sizes up to 1.5 GB" [1] p. 17) while its own eight slots top out
at 1 GB with 64-megabit DIMMs [1] p. 16, Table 4-3 p. 41. (The 9500 note elsewhere says "up to
2 GB" in its own Hammerhead section [2] p. 10 — a contradiction internal to that document;
§5.)

### 3.2 DRAM organization

**The array.** Hammerhead controls a 128-bit-wide DRAM array [1] p. 17. The width is built from
pairs: all RAM is provided by DRAM on 8-byte (64-bit) DIMMs, and "when the startup software
detects two DIMMs that contain the same amount of memory, it configures their combined memory
as a single bank with a memory data bus 128 bits wide" [1] §"Random-Access Memory" p. 16. The
9500 note describes the same mechanism from the controller's side: "Even though the system data
bus is only 64 bits wide, the memory controller in the Hammerhead IC can support data read
operations 128 bits wide by interleaving the data from two DIMM slots. When the startup software
detects two DIMMs that are the same size in adjacent banks (Bank 0–Bank 1), it enables
interleaving for that pair of DIMMs" [2] §"Data Interleaving" p. 10.

**Interleaved transfers.** "For an interleaved transfer, the memory controller cycles both banks
at the same time into a 128-bit buffer, then transfers data 64 bits at a time to the system data
bus. The memory controller determines which bank's data to transfer first on the basis of address
signals that carry the critical word-first information. The controller supports both the
critical quad-word first behavior of the PowerPC 601 and the critical double-word first behavior
of the PowerPC 603 and PowerPC 604" [2] p. 10. Interleaving is enabled per bank pair through the
bank base register control bits (§2.10) and requires identical DIMMs in the corresponding slots.

**Devices.** The controller supports 1-, 4-, 16- and 64-megabit DRAM devices with an access
time (T_RAS) of 70 ns or less [1] §"DRAM Devices" p. 43, [2] p. 31. Bank sizes run from a 4 MB
minimum to a 64 MB maximum, and the largest DIMM supported is a two-bank DIMM holding 128 MB
[1] §"RAM DIMM Configurations" p. 41, [2] p. 26. Device count is constrained by the load limits
of the unbuffered signals: at most two devices per data line and at most eight devices per /RAS
line on each DIMM [1] p. 43.

**Refresh.** Hammerhead provides a CAS-before-RAS refresh cycle every 15.6 µs; the parts must
be compatible with it — the note's example is that the cycle refreshes 2K-refresh parts within
32 ms [1] §"RAM Refresh" p. 43, [2] p. 31.

**The DIMM connector.** Each DIMM slot carries a 12-bit multiplexed address bus A(13:0) (with
B(0), an alternate of A(0) that allows a DIMM to split the 64-bit bus into two 32-bit halves —
tied to A(0) on the TNT main logic boards), eight /CAS lines, four /RAS lines, DQ(63:0), byte-
pair /OE and /WE lines [1] Table 4-2. Two facts about those pins matter to anyone modelling
the sizing flow: the JEDEC presence-detect signals PD(8:1) and /PDE and the identification
signals ID(1:0) are **not used** on these machines, and no +3 V power is provided on the
connector [1] notes to Table 4-2. Sizing therefore cannot come from the slots' identity pins;
the notes never say how the startup software actually detects DIMM sizes (§6.3).

**Timing selection.** The Network Server note documents a behaviour the TNT notes omit: "The
boot ROM sets DRAM timing based on two factors: the detected bus speed and detected parity. If
parity is detected, 60 ns timing is set. If parity is not detected, 70 ns timing is set. At
50 MHz, 70 ns timing is approximately a 20% memory bandwidth penalty" [3] §2.4.1. Parity itself
is not a Hammerhead function: it is implemented by "a modification of the PowerMac 9500 data
path chip", which "writes and reads byte-wide parity for memory accesses only. ROM and SRAM are
not parity protected" [3] §2.4.1.

### 3.3 Address multiplexing

Different DRAM device types require different row/column splits. Hammerhead supports **two
addressing modes, selected individually for each bank** through the bank base register control
bits [1] §"RAM Address Multiplexing" p. 41, [2] p. 30. The controller does not support devices
requiring more than 12 row address bits [1] note to Table 4-4.

Table 4-4 of the developer note, address multiplexing modes by device [1] p. 41 (identical in
[2] p. 30):

| Device size | Device type | Row address bits | Column address bits | Address mode |
|---|---|---|---|---|
| 4 Mbit | 1M × 4 | 10 | 10 | 1 |
| 4 Mbit | 512K × 8 | 10 | 9 | 1 |
| 4 Mbit | 256K × 16 | 10 | 8 | 0 |
| 16 Mbit | 4M × 4 | 11 | 11 | 1 |
| 16 Mbit | 4M × 4 | 12 | 10 | 1 |
| 16 Mbit | 2M × 8 | 11 | 10 | 1 |
| 16 Mbit | 2M × 8 | 12 | 9 | 0 |
| 16 Mbit | 1M × 16 | 12 | 8 | 0 |
| 64 Mbit | 16M × 4 | 12 | 12 | 0 |
| 64 Mbit | 8M × 8 | 12 | 11 | 0 |
| 64 Mbit | 4M × 16 | 11 | 11 | 1 |
| 64 Mbit | 4M × 16 | 12 | 10 | 1 |

Table 4-5, the resulting signal mapping on the DRAM_ADDR bus [1] p. 42:

| | A(11) | A(10) | A(9) | A(8) | A(7) | A(6) | A(5) | A(4) | A(3) | A(2) | A(1) | A(0) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **Mode 0** row | A22 | A21 | A20 | A19 | A18 | A17 | A16 | A15 | A14 | A13 | A12 | A11 |
| **Mode 0** column | A26 | A25 | A24 | A23 | A10 | A9 | A8 | A7 | A6 | A5 | A4 | A3 |
| **Mode 1** row | A24 | A23 | A21 | A20 | A19 | A18 | A17 | A16 | A15 | A14 | A13 | A12 |
| **Mode 1** column | A25 | A24 | A22 | A11 | A10 | A9 | A8 | A7 | A6 | A5 | A4 | A3 |

### 3.4 L2 cache operation

The L2 cache is a single SIMM holding 256 KB, 512 KB, 1 MB or 4 MB [1] §"Second-Level Cache
SIMM" p. 45. Its organization, as stated by both notes:

- **Write-back**, **direct mapped** (single set), **allocate on read or write** [1] p. 17, p. 45;
  [2] §"Second-Level Cache" p. 10.
- It "allocates on read operations from ROM addresses and from main RAM addresses responded to
  by the Hammerhead memory controller" [1] §"L2 Cache Operation" p. 45 — that is, it caches the
  two regions Hammerhead itself slaves for, and nothing else.
- It "allocates only on burst read and write operations that miss. For nonburst read operations
  that hit, the data is read from the cache. For nonburst write operations that hit, the
  specified line is written back to memory before the single cycle of write data is written back
  to memory" [1] p. 45.
- A hit requires the tag-store contents at the given index to match the address *and* the tag
  valid bit to be set [1] p. 45.

The stores are asynchronous with each other: the data store uses synchronous burst SRAM (access
time 11 ns or less on the 7500/8500 and 9500 [1] p. 45, [2] p. 10), and the tag store uses
standard SRAM [1] p. 17. The same SIMM serves both the 7500 and the 8500, "but with some
differences in operation, as specified in the next section" [1] note p. 45 — and the next
section's differences are the *processor's* timing (604 vs 601, below), not the SIMM's.

The SIMM connector is a 160-position part whose signals read as a direct tap of the AR bus and
the cache control plane [1] Tables 4-6, 4-7 pp. 46–49:

| Signal | Description |
|---|---|
| ArDat(63:0) | Processor data bus |
| BufAdr(27:10) | Address bus, buffered |
| TAG(13:0) | Tag store value |
| TAGVALID / TAGDIRTY | Valid bit / dirty bit from the tag store |
| TAGOEN / TAGWEN | Tag store output enable / write enable |
| DATAOEN / DATAWEN | Data store output enable / write enable |
| BURSTADV | Burst advance signal to data store SRAMs |
| XFRSTART | Transfer start signal to data store SRAMs |
| CacheSysClk | System clock |
| CachePrsnt | Active (low) if an L2 cache card is installed |
| CacheSize(1:0) | b00 = 512 KB, b01 = 256 KB, b10 = 1 MB, b11 = 4 MB |

On the Network Server the cache DIMM is "fit, form and function compatible with the PowerMac 8500
cache slot" [3] §6.2, with tighter SRAM guidance — tag no slower than 8 ns (7 ns preferred),
cache memory 11 ns or better [3] §2.4.2, §6.2 — and one documented electrical erratum: "early
versions of burst SRAM from Motorola may cause signal integrity issues within the DIMM itself,
resulting in clocking glitches that cause failed burst writes"; later-generation 5 V parts or
3.3 V parts from Motorola, Micron or IBM are preferred [3] §6.2.

### 3.5 L2 and processor-bus timing

The controller "makes data available early in the timing process by taking advantage of the
following system attributes" [1] §"L2 Cache Operation" p. 45:

- "With only one processor, there is only one snooping mode so the **ARTRY** (address retry)
  signal is never asserted for a microprocessor access that hits in the L2 cache."
- "By using the **DBDIS** (data bus disable) signal defined for the PowerPC 604, the cache
  controller is able to assert **DBG** in the same cycle that **TS** asserts, so the processor
  can accept data (**DBB**) in the very next cycle. The DBDIS signal allows the L2 cache RAM
  devices to be enabled, driving the data bus before the direction of the access (read or
  write) has been determined."
- "The **DRTRY** (data retry) signal is never asserted, so the controller can use non-aging
  mode with the PowerPC 604."

The resulting latency, by processor: "With the PowerPC 604, data is made available, that is, the
**TA** (transfer acknowledge) signal is asserted, in the next clock cycle after the assertion of
TS (transfer start). With the PowerPC 601, data is made available as early as the second clock
cycle after the assertion of TS" [1] p. 45.

### 3.6 Arbitration and coherency

The system bus is "a multiple master, multiple caching agent shared bus running at up to
50 MHz" [3] §5.1. The arbitration definitions the Network Server note publishes are the only
ones any document in evidence gives for this bus:

- Each master drives a point-to-point **`master BusReqL`** to the arbiter and "a bus request is
  kept asserted until the master has received a qualified bus grant (*master* BusGrantL asserted
  and sysAbbL deasserted)" [3] §5.1.1.
- The arbiter returns one **`master BusGrantL`** per master; grants must be qualified by the
  deassertion of `sysAbbL`, and "there is bus parking on the address bus" [3] §5.1.1.
- Data-bus tenure is arbitrated in parallel: the arbiter drives one **`DbgL`** per master and one
  **`SsdL`** (slave source/sink data) per slave, with the shared **`sysDBWOL`** telling a master
  that its next tenure will be a write [3] §5.1.1.
- Each slave returns a point-to-point **`Rddal`** (read data available) to the arbiter; "each
  slave returns data in the order it is requested; however, within a request the critical word
  is returned first" [3] §5.1.1.

On the ANS board the arbiter's pin budget is **seven masters and eleven slaves** [3] §5.1.1 —
that is, the arbiter device brings out seven `BusReqL` inputs, seven `BusGrantL`/`DbgL` outputs
and eleven `Rddal`/`SsdL` lines. Whether the arbiter function lives inside Hammerhead is not
stated in so many words by any document; MkLinux parking `ArbConfig`, `WhoAmI` and `IntReg`
inside the Hammerhead window (§2.5–§2.7) is the strongest indication that it does (*inferred*;
§6.10). The TNT machines' own master count is not documented anywhere.

Coherency: the granularity unit is **32 bytes**, and "level 2 caching agents force inclusion on
the L1 of the 60x processors" [3] §5.1 — the L2 contains the L1 by construction, which is what
makes single-snoop-mode operation (§3.5) sufficient on a uniprocessor. With more than one
caching agent the single-snooping-mode premise fails, which is the "hooks for more than one
caching agent" the ANS bus adds [3] §5.

The PCI side's arbitration is a separate function and does not belong to Hammerhead: on the
7500/8500, "a separate logic device (gate array) provides the priorities for bus arbitration as
follows: 1. Grand Central IC (I/O device controller; highest priority) 2. PCI slots and Bandit
master, in round-robin sequence: that is, each in turn, with equal priority" [1] §"Bandit PCI
Bridge IC" p. 18; the 9500 note repeats it for its two bridges [2] p. 11. See
[bandit.md](bandit.md).

### 3.7 Reset and exceptions

A processor out of hardware reset fetches $FFF00100 through Hammerhead's ROM decode, and the
first code executed determines primary from secondary (§4.5) [3] §3. On the Network Server the
processor-card hard reset "de-asserts approximately 500 ms after Main Logic Board hard reset"
[3] §6.1.

Hammerhead's exception contribution is narrow. Memory parity errors — generated by the modified
data-path chip, not by Hammerhead (§3.2) — "generate **MCP** to the 60x. This may or may not be
gracefully handled by the operating system, however it should normally prevent further
execution" [3] §5.2; the `ParityErrL` signal is present on the processor connector and "would
normally be routed to MCP on the processor" [3] §6.1. The inter-processor interrupt of §2.7 is
the other path. No document attributes Hammerhead a transfer-error (TEA) assertion, a bus
timeout, or any other exception behaviour; the fault semantics of the *siblings'* windows are
Bandit's and Grand Central's, not Hammerhead's (§6.11).

## 4. Programming model

### 4.1 Who programs Hammerhead

Almost nobody, and that is the platform's design. The developer notes' phrase is "the system
software initializes the address mode bits in the bank base registers as part of the process of
determining the amount of RAM installed" [1] p. 41 — but on these machines "system software"
means the ROM firmware: Open Firmware performs the hardware bring-up and sizes memory before any
operating system runs, and every operating system then takes memory size, clock rates, L2
presence and CPU count from the Open Firmware device tree rather than from the chip [4]. The
`/memory` node's `reg` property is the single source of truth for RAM size; the CPU node carries
`clock-frequency`, `bus-frequency` and `timebase-frequency`; the `l2-cache` node carries the
cache geometry; `/chosen` and the CPU-count logic consume +$90/+0xC0 [4]. The one operating
system codebase known to touch Hammerhead registers at all is MkLinux's multiprocessor plug-in
(§4.5) [5].

### 4.2 Cold boot order

The boot flow of the 4 MB mask ROM (two August 1995 releases; they differ only in the
diagnostics object and the 68k Toolbox image, the firmware components being byte-identical)
[4]:

1. **Reset fetch.** The processor comes out of reset at $FFF00100, inside Hammerhead's ROM
   decode, and enters the NanoKernel's reset entry [3] §3, [4].
2. **NanoKernel bring-up.** The kernel builds its BAT and page-table mappings from the ROM's
   configuration record: ROM at $FFC00000 (and overlaid at zero for early boot), the firmware
   image at $FF800000 logical / $00400000 physical, and the I/O region 1:1 and cache-inhibited
   [4].
3. **Open Firmware** (version 1.0.5) runs, probes Hammerhead, both Bandits, Chaos and Grand
   Central, sizes memory, builds the device tree, and sets up `/chosen` [4].
4. **POST** (the ROM's diagnostics object — entry points `.POST`, `.L2DataCacheTest`,
   `.SerialTestManager`) runs its checks, logging results into NVRAM through Grand Central's
   bank-select and data ports, and executes the L2 test of §4.4 [4].
5. **`boot /AAPL,ROM`** loads the Mac OS boot path, the **68k emulator** starts, and the Toolbox
   ROM runs its machine identification (§4.3), low-memory setup and driver loading [4].
6. **Native drivers** load through the Name Registry and driver loader [4].

The identity reads of §4.3 happen at steps 3 and 5, and the L2 registers are touched at steps 3
and 4. Everything else in the window belongs to memory sizing (§2.10).

### 4.3 Machine-identity dispatch

One register, +$00, decides which of the two hardware platforms — and then which machine within
the platform — the single shared ROM will bring up.

**The 68k identification routine** (at $FFC14844) synthesizes a 16-bit software CPU ID and
matches it against the ROM's per-product records [4]:

```
CMPI.B  #$39,$F8000000        ; Hammerhead ID first byte $39 -> TNT family
  AND.L #$40000000,$F8000020  ; Hammerhead +$20 bit 30 set  -> $3020 (9500)
  AND.L #$00080000,$F301A000  ; BoxID bit 11 (big-endian read) set -> $3022 (8500)
  else                        ;                                 -> $3021 (7500)
else:
  swap($F8000000) == $3001    ; -> $3001 (7200 / Catalyst)
```

The synthesized ID selects one of four product records, each carrying the machine's
low-level vector, RAM-info and device-decoder tables; the resulting per-product flags read $3E
on the 7500, $3F on the 8500 and $3D on the 9500, and $66 on the Catalyst path (*observed* on
all four personalities) [4]. Note that the dispatch consumes exactly three facts from
Hammerhead — byte 0 of +$00, bit 30 of +$20 — plus one bit of Grand Central's BoxID; the
BoxID's other bits do not participate in the model selection on this path [4].

**Open Firmware's decode** runs earlier and produces the root node's `AAPL,cpu-id` and
`compatible` properties. Decoded from the firmware image's own token stream [4]:

```
case (long@(F8000000) >> 24):            ; Hammerhead ID byte 0
of $39:                                  ; TNT family
  AAPL,cpu-id =
      ($39 << 16
       | (((byte@(F8000020) >> 5) | ((byte@(F8000020) >> 1) & 8)) << 4
          | (byte@(F8000030) >> 4)) << 8
       | ($80 if PVR>>16 != 1)           ; not-a-601 flag
       | (halfword@(F301A000) >> 11)     ; BoxID bits 11-15
of $30:                                  ; Catalyst
  Catalyst flag := true
  AAPL,cpu-id = halfword@(F301A000) >> 11
```

The `compatible` selection then keys on the model selector computed from +$20's top byte
(§2.3): selector 4 yields `AAPL,7500` or `AAPL,8500` — split by BoxID bit 13, set meaning 7500 —
selector 2 yields `AAPL,9500`, and anything else yields `AAPL,????` with no display nodes
instantiated [4]. The Catalyst arm of the $30 path is labelled `AAPL,7300` in this firmware,
not `AAPL,7200` [4] — the 1995 image reuses the 7300 string for the Catalyst machine, and no
release of this firmware lineage ever emits `AAPL,7200` [4].

### 4.4 L2 bring-up and the power-on test

The firmware's L2 flow, from the decoded `.L2DataCacheTest` [4]:

1. Read +$E0; isolate bit $80; if clear, skip the entire test — this is the no-SIMM case, where
   the pulled-up presence pin has already made Hammerhead "disable all external cache
   operations" [1] p. 17.
2. Write 0 to +$E0; read it back and keep the low three bits: 0 → 512 KB, 1 → 256 KB, 2 → 1 MB,
   3 → 4 MB; any other value exits with an error.
3. Write $80 to +$F0, read a memory range the size of the cache (fill/flush), write 0 to +$F0.
4. Write $70 to +$E0 — the enable/mode word whose bits no source names (§6.7).

The size read matches the SIMM's `CacheSize(1:0)` strap codes exactly (§2.8). The firmware
publishes the result in the device tree: the `l2-cache` node is built beside the CPU node's
cache-size properties, and its construction is where the $F80000E0 literal appears [4].
Operating systems read the node, not the register.

### 4.5 Multiprocessor bring-up

The dual-processor configuration — the 9500-class dual-604 card, whose software interface the
Network Server shares ("the way software accesses the dual processor hardware implementation is
the same as in the PowerMac 9500 family" [3] §3) — boots like this:

1. Both processors come out of hardware reset and fetch $FFF00100 through Hammerhead [3] §3.
2. The first code executed determines whether the processor is the primary — and continues with
   machine initialization, POST and Open Firmware — or the secondary, which "will enter a
   spin-wait for an interprocessor interrupt" [3] §3. The determination reads `WhoAmI`
   (+$B0): $10 primary, $08 secondary [5] (*inferred* as to which code performs the read; the
   register's values and their meaning are attested by the header).
3. Enabling the second processor "will be operating system dependent" [3] §3. The operating
   system's tool is `IntReg` (+$C0): $80 = `SecInt`, signalling the secondary [5].
4. The secondary signals the primary through Grand Central external interrupt 10 on the Network
   Server, raised by an access to the Ethernet PROM's chip-select space — there is no reverse
   register in Hammerhead [3] §4.5.
5. The timebases can be synchronized in software: the Network Server implements a
   `Synchronize_TimeBase` register (bit 15 of the LCD GBUS device's register 2, at offset
   $1C020) — write 0 to stop the 604 timebases, write zero to each processor's TB, write 1 to
   restart them in lock-step; "this may or may not be useful in two processor configurations"
   [3] §4.6.1. (That register is a board device, not a Hammerhead one.)

MkLinux's MP plug-in is the surviving OS-side consumer of the register set: it maps Hammerhead
alongside Bandit 1, the Bandit 1 config port, Grand Central and the Ethernet PROM, passes the
Hammerhead mapping into the processor-installation call, and names `ArbConfig`/`TwoCPU`,
`WhoAmI`/`PriCPU`/`SecCPU` and `IntReg`/`SecInt` as the constants of the two-way board [5]. The
header's own comment scopes it: "this code is specific to a single MP hardware type: the
Apple/DayStar 2-way board... the MP support is intended to be an extension of the hardware, not
an I/O device" [5].

### 4.6 What the ROM's 68k half records

The 68k `DecoderInfo` tables — the per-product device base-address tables the Toolbox ROM uses —
are each preceded by a longword $F8000000, recording the north-bridge base for the platform, on
both the TNT and the Catalyst table [4]. The 68k half therefore knows where Hammerhead lives
but never touches its DRAM control: memory sizing is finished before the 68k emulator starts,
and the Toolbox ROM's own RAM global comes from the firmware's configuration record [4].

## 5. Quirks & errata

- **The identification register is a whole-platform personality switch.** Serving $3001xxxx at
  +$00 makes the machine take the Catalyst personality — the 7200's decoder table, Platinum
  video, no MESH. The value is not "an ID the ROM requires"; it selects the *other* platform
  (§2.2).
- **The same register is read at two different widths.** The 68k dispatch tests byte 0 ($39 =
  TNT); the Catalyst arm and the POST test the leading *halfword* ($3001). Both are the same
  longword at +$00 [4] (§2.2).
- **+$20 gates the display subsystem.** An unrecognized top byte does not merely mislabel the
  machine — Open Firmware builds no `chaos`/`control` nodes at all, and a TNT board that answers
  the identity with anything but $80/$40 in the top byte boots with no onboard video (§2.3).
- **The Catalyst machine publishes as `AAPL,7300`.** The 1995 firmware's Catalyst arm emits the
  7300 string; no release of this firmware lineage emits `AAPL,7200` (§4.3) [4].
- **The 7200 shares the window with a different chip.** Platinum (343S1184) sits at $F8000000 on
  Catalyst and combines memory control *and* video timing; Hammerhead offsets must not be
  ported there, and Platinum's must not be ported here (§1.2, §2.1) [4].
- **Nothing in the DRAM control is where the DIMMs say it is.** Presence-detect and ID pins are
  unused on the RAM DIMM connectors; sizing is a pure firmware exercise whose algorithm no
  document describes (§3.2, §6.3) [1] notes to Table 4-2.
- **One slow DIMM slows the machine.** On the Network Server, detecting a non-parity DIMM drops
  *all* memory to 70 ns timing — about a 20% bandwidth penalty at 50 MHz — rather than merely
  disabling parity (§3.2) [3] §2.4.1.
- **Parity is not Hammerhead's.** It lives in a modified 9500 data-path chip and covers memory
  accesses only; ROM and SRAM are unprotected (§3.2) [3] §2.4.1.
- **"Fast L2" is keyed on the bus clock, not the processor.** The Network Server ROM enables it
  for bus speeds of 44 MHz and below, buying one cycle of L2 latency — so a 132 MHz card (44 MHz
  bus) gets it and a 150 MHz card (50 MHz bus) does not, and an upgrade card can *remove* the
  mode while raising the core clock [3] §2.4.2, §6.2.
- **Early Motorola burst SRAM is an erratum.** Signal integrity issues inside the cache DIMM
  cause clocking glitches and failed burst writes; later 5 V parts or 3.3 V parts from Motorola,
  Micron or IBM are the fix [3] §6.2.
- **ARTRY never fires on a uniprocessor.** One processor means one snooping mode, so address
  retry is never asserted for a processor access that hits in the L2; DRTRY is likewise never
  asserted, allowing non-aging mode with the 604 (§3.5) [1] p. 45.
- **The L2 does not cache everything.** It allocates only from ROM addresses and from main-RAM
  addresses Hammerhead itself answers — everything behind Bandit or Chaos never reaches it
  (§3.4) [1] p. 45.
- **The 9500 developer note contradicts itself on the memory ceiling.** The RAM section says
  the controller supports up to 1.5 GB; the Hammerhead section of the same note says up to 2 GB
  [2] pp. 9–10. The 7500/8500 note's 1.5 GB figure [1] p. 17 and the 9500's twelve-slot ×
  128 MB arithmetic both support 1.5 GB (§3.1).
- **The Network Server ROM decodes less memory than the controller supports.** Hammerhead
  handles 1.5 GB; the ANS production ROMs decode up to 512 MB, and exceeding it fails in the RAM
  test rather than cleanly [3] §2.4.1.
- **Interleaving demands twins.** Only same-size DIMMs in the corresponding slots pair into a
  128-bit bank; a single odd DIMM leaves half the array's width unused (§3.2) [1] p. 16, [2]
  p. 10.
- **The register file is byte-granular on $10 centres.** The known registers are single bytes
  at offsets that are multiples of $10, inside a window whose 32-bit-centred structure is
  inferred from Platinum (§2.1).
- **Hammerhead is big-endian; everything behind Bandit is little-endian.** The POST code reads
  the identifier with a plain `lwz`, while the BoxID register in Grand Central must be read
  byte-reversed (§1.1, §4.3) [4].

## 6. Open questions

1. **The full register map.** Nine offsets are attested (§2) and one range is observed (§2.10);
   the rest of the 2 KB window is undescribed by any document, and whether unattested offsets
   alias onto the named registers is unknown. The $10-centre/128-register structure itself is
   an inference from Platinum's documented geometry.
2. **The bank base register layout.** The 9500 note names the fields — base address,
   address-multiplexing mode, interleave enable — but no source gives the register
   count, width, stride or bit packing. The firmware's sizing pass is observed to write and
   read back a file spanning +$1C0 through +$4F0 (§2.10); how many banks that covers (four bank
   pairs on an 8-slot machine, six on a 12-slot machine) and where each field sits is open.
3. **The DIMM sizing algorithm.** Presence-detect and ID pins are unused, yet the startup
   software "detects" DIMM sizes and pairing. The probing method — test writes, timing
   discrimination, or something else — is published nowhere.
4. **DRAM timing selection on the TNT machines.** The 60 ns/70 ns by-parity-and-bus-speed rule
   is documented only for the Network Server (§3.2). Which registers carry the timing, and
   whether the 7500/8500/9500 ROMs apply the same rule, is unknown.
5. **"Fast L2" mode's register.** The behaviour is documented (enabled at ≤44 MHz, one cycle
   less latency) but no source names the bit that selects it, or whether the TNT ROMs carry an
   equivalent.
6. **+$F0's second role.** The address appears in the firmware's `bandit` node construction,
   among the PCI bridge's address-allocation code — a context the L2 flush/fill strobe reading
   does not explain (§2.9).
7. **The $70 mode bits of +$E0.** Written as a unit at the end of the power-on L2 test; no
   source names them (§2.8).
8. **+$30's real content.** Only its consumption — the top byte folded into `AAPL,cpu-id` — is
   attested; no document states what a board returns there, or what the rest of the register
   holds (§2.4).
9. **The unattested bytes of +$00 and +$20.** The dispatch consumes byte 0 and the top byte
   respectively; the remaining bytes of both registers are unconstrained by any evidence.
10. **Where the arbiter lives, and the TNT master count.** The ANS arbiter pin budget is seven
    masters and eleven slaves; the TNT machines' counts are undocumented, and the identification
    of Hammerhead as the arbiter rests on MkLinux's placement of `ArbConfig` inside its window
    (§3.6) — *inferred*, not stated.
11. **Hammerhead's fault behaviour.** Whether the chip asserts TEA or any timeout for illegal
    accesses, beyond the parity-to-MCP path, is unattested; the recoverable-fault contracts on
    this platform belong to Bandit and Grand Central.
12. **343S1142 versus 343S1190.** The 7500/8500/9500 firmware names 343S1142; secondary
    Network Server material gives 343S1190. Whether the latter is a revision, an ANS-specific
    part, or an error is unresolved.
13. **The 1-megabit device row.** The DRAM Devices sections name 1-megabit-class devices among
    those supported, but the multiplexing table covers only the 4-, 16- and 64-megabit sizes
    (§3.2, §3.3).
14. **The 1997 Hammerheads.** The 7300/7600/8600/9600 reuse the TNT identity decode
    byte-identically [4], but whether their boards carry a revised Hammerhead part is not
    documented in the evidence set.
15. **Uniprocessor `ArbConfig` and `WhoAmI` values.** No source states what a single-processor
    machine returns from +$90 and +$B0; the $10-primary reading for `WhoAmI` is inference
    (§2.5, §2.6).

## References

1. Apple Computer, Inc., *Developer Note: Power Macintosh 7500 and Power Macintosh 8500
   Computers*, Developer Press, 1995 — Chapter 2 "Architecture": §"Random-Access Memory" p. 16
   (eight DIMM slots, 1 GB, same-size DIMM pairing into a 128-bit bank), §"Second-Level Cache"
   p. 17 (8500 ships 256 KB; SIMM size sensing by two connector pins; pull-ups disable external
   cache), §"Hammerhead Memory Controller IC" p. 17 (the four components; 128-bit array; 1.5 GB),
   §"Bus Bridge" p. 17 (PCI 33 MHz asynchronous, processor bus 50 MHz), §"Bandit PCI Bridge IC"
   p. 18 (bridge function; PCI arbitration priorities), §"Big-Endian and Little-Endian Bus
   Addressing" p. 18; Chapter 4 "Expansion Features": Table 4-2 and notes pp. 37–38 (DIMM
   signals; PD/ID unused; no +3 V), Table 4-3 p. 41 (bank 4–64 MB; 128 MB two-bank DIMM),
   §"RAM Address Multiplexing" p. 41 and Tables 4-4/4-5 pp. 41–42 (two per-bank addressing
   modes and the DRAM_ADDR mapping), §"DRAM Devices" p. 43 (device sizes; T_RAS ≤ 70 ns; load
   limits), §"RAM Refresh" p. 43 (CAS-before-RAS every 15.6 µs), §"Second-Level Cache SIMM"
   p. 45 (sizes; 11 ns sync burst SRAM), §"L2 Cache Operation" p. 45 (write-back, direct mapped,
   allocation and hit behaviour; ARTRY/DBDIS/DBB/DRTRY; 604 and 601 TA timing), Tables 4-6/4-7
   pp. 46–49 (cache SIMM connector pins and signals, CacheSize and CachePrsnt codes); Glossary
   ("Hammerhead").
2. Apple Computer, Inc., *Developer Note: Power Macintosh 9500 Computers* (Power Macintosh
   9500/120 and 9500/132), Developer Press, 1995 — Chapter 2 "Architecture": §"Random-Access
   Memory" p. 9 (2 GB allocated, 1.5 GB supported, contiguous from $00000000; twelve slots),
   §"Second-Level Cache" p. 10 (built-in 512 KB; organization), §"Hammerhead Memory Controller
   IC" p. 10 (scope; 2 GB statement), §"Data Interleaving" p. 10 (64-bit system bus; 128-bit
   interleaved reads; critical word first for 601/603/604), §"Bank Base Registers" p. 10 (base
   address plus control bits; address mode and interleave enable), §"Bandit Bus Bridge ICs" and
   §"Bus Clock Rates" p. 11 (two Bandits; PCI 33 MHz; processor bus 40/44 MHz, one third of
   processor speed); Chapter 4 "Expansion Features" pp. 26–31 (RAM DIMM configurations and
   connectors, address multiplexing, DRAM devices, RAM refresh).
3. Apple Computer, Inc., *Network Server Hardware Developer Notes*, c. 1996 — §2.4.1 "Parity
   Memory" (data-path chip parity; 60/70 ns timing rule; 20% penalty; 512 MB ROM decode limit;
   eight slots), §2.4.2 "Cache Memory" (8500-compatible cache DIMM; fast L2 ≤ 44 MHz; SRAM
   speeds), §2.10 "Low-Skew Clocking" (motherboard acquires clocking from the processor card;
   40–50 MHz system speeds), §2.11 "Processor Cards" (dual-CPU form factor), Chapter 3
   "Network Server Initialization" (reset vector $FFF00100 fetched through Hammerhead;
   primary/secondary split; spin-wait for interprocessor interrupt; POST and Open Firmware map
   and test main memory "in conjunction with Hammerhead Registers"; L2 mapped, zeroed,
   enabled), §4 "Network Server Address Map" (ARBus-PCI bridges; Grand Central register
   mapping), §4.5/§4.6.1 (SecToPri_Int via Ethernet PROM chip select; Synchronize_TimeBase at
   GBUS $1C020 bit 15), §5 "Network Server System Bus" (60x-bus signal set; agent types;
   32-byte coherence; L2 forces L1 inclusion; arbitration signal definitions with 7-master/
   11-slave pin counts; bus parking), §5.2 "Parity Handling" (parity errors generate MCP), §6.1
   "Processor Slot Definition" (ArAdr/ArDat/ArTS/ArTA/ArAack/ArArtry/ArTea/ArTbst/ArCI/ArDBB/
   SecInt_1/PriBG_1/SecBG_1/ParityErrL/DBDIS_1 pins; refClk and clkToProc timing; 500 ms reset
   deassertion), §6.2 (burst-SRAM erratum; fast L2; tag RAM speeds).
4. Power Macintosh 7200/7500/8500/9500 boot ROM, August 1995 — 4 MB mask ROM in two releases
   (header checksums $96CD923D and $9630C68B; `BootstrapVersion` "Boot TNT 0.1p"), comprising
   the 68k Toolbox ROM, the PowerPC exception table, the NanoKernel, the POST/diagnostics
   object (source-named entry points `.POST`, `.L2DataCacheTest`, `.SerialTestManager`) and the
   Open Firmware 1.0.5 image. Evidence used: the decoded Open Firmware token stream — the
   `hammerhead` node with `model "AAPL,343S1142"`, the `platinum` node's `reg F8000000 00000800`
   plus framebuffer `F1000000 01000000`, the `l2-cache` node's $F80000E0 literal, the `/chosen`
   node's $F8000090/$F80000C0 literals, the `bandit` node's $F80000F0 literal, the
   `aliases` strings `/chaos@F0000000`, `/bandit@F2000000`, `/bandit@F4000000`, the
   `compatible` list and the model-decode token sequence; the disassembled POST object — the
   +$00 halfword-$3001 boolean test, the `.L2DataCacheTest` register sequence on +$E0/+0xF0;
   the disassembled 68k machine-identification routine at $FFC14844; the two 68k `DecoderInfo`
   tables, each preceded by the longword $F8000000; the byte-identical Open Firmware image of
   the 1997 TNT ROMs (same identity decode; the 8600 reports `AAPL,8500`, the 9600
   `AAPL,9500`); boot-time observation of the memory-sizing write/read-back pass over
   +$1C0–+$4F0 and of the per-model identity outcomes.
5. Apple Computer, Inc. and Open Software Foundation, MkLinux for Power Macintosh, source code
   distribution (copyright notices 1991–1998) — `POWERMAC/mp/MPPlugIn.h`: the register
   definitions `HammerHead 0xF8000000`, `ArbConfig 0x0090` (`TwoCPU 0x02`), `WhoAmI 0x00B0`
   (`PriCPU 0x10`, `SecCPU 0x08`), `IntReg 0x00C0` (`SecInt 0x80`), alongside `Bandit1`,
   `PCI1AdrReg 0xF2800000`, `GrandCentral 0xF3000000`, `EtherNetROM 0xF3019000`;
   `POWERMAC/mp/mp.c`: the Hammerhead mapping in the processor-probe path, the byte-wide read of
   `IntReg`, and the comment scoping the plug-in to the Apple/DayStar two-way board.
6. The NetBSD Foundation, NetBSD/macppc source tree — `sys/arch/macppc/dev/platinumfbreg.h` and
   `platinumfb.c`: the Platinum register geometry at the shared $F8000000 base (128 registers,
   32-bit, on $10 centres, `PLATINUM_REG_COUNT = 128`), and the driver's record of the
   controller's Open Firmware properties (`name platinum`, `model AAPL,343S1184`, `reg
   F8000000 00000800` and `F1000000 01000000`), used as the structural comparison for
   §2.1.
