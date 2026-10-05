# The AV family — Macintosh Quadra 840AV and Macintosh Centris/Quadra 660AV

**Contents:**

1. [Overview & membership](#1-overview--membership) — what the family is, the boards and speed grades, what every
   board carries, what none of them carries
2. [Board architecture common to the family](#2-board-architecture-common-to-the-family) — the ASIC set; the
   YMCA memory controller and arbiter in full (register file, machine identification, speed and DRAM
   programming, RAM sizing, the video routing bits); DRAM rules; the boot ROM; VRAM; the DAV connector
3. [Memory map & address decode shared by the family](#3-memory-map--address-decode-shared-by-the-family) — the
   32-bit physical map, the I/O island and its alias, device bases, the 24-bit compatibility map, the startup
   overlay, the error contract
4. [Device roster](#4-device-roster) — every part on the board, its base address and its page
5. [Interrupt, bus, and clock architecture](#5-interrupt-bus-and-clock-architecture) — the seven interrupt
   levels, the four CPU-bus masters and their arbiter, the I/O bus, NuBus and the MUNI bridge in full, the
   clock tree
6. [Per-machine index](#6-per-machine-index) — the Quadra 840AV, the Centris/Quadra 660AV, the two unshipped
   speed grades, and what is deliberately identical
7. [Open questions](#7-open-questions)

---

## 1. Overview & membership

### 1.1 What the family is

The AV family is Apple's Cyclone/Tempest generation: two shipping machines — the **Macintosh Quadra 840AV**
(board codename *Cyclone*) and the **Macintosh Centris 660AV** (board codename *Tempest*; renamed **Quadra
660AV** late in its life) — that are "essentially the same circuit board and system components, with variations
as noted" [1] p. 10. Both are built around a 32-bit Motorola MC68040 running at 40 MHz (Quadra 840AV) or 25 MHz
(Centris 660AV), and both add, on one board, the three things that define the platform:

- "an input/output (I/O) subsystem that runs independently of the main processor" — the **Peripheral Subsystem
  Controller (PSC)**, whose nine DMA channels move data for SCSI, Ethernet, both serial ports, the floppy drive
  and both sound directions without processor intervention ([PSC](psc.md) is its page);
- "an independent digital signal processor (DSP) subsystem that supports real-time processing of data" — an
  AT&T **DSP3210** sitting on the CPU bus as a fourth bus master, executing its programs out of main memory
  ([DSP board](dsp3210-board.md), [DSP3210](../../hardware/dsp3210.md));
- built-in audio-visual I/O: 16-bit stereo sound through the **Singer** codec at up to 48 kHz, television-standard
  composite/S-video output, and a full video-capture chain (CIVIC, Sebastian, the Philips video-in pair) — the
  "AV" in the products' names [1] pp. 3, 5–6.

The two machines were introduced in mid-1993; the shared boot ROM's universal declaration data carries the
build date "Friday, June 25, 1993" [3] (*observed* in the ROM image), and the same 2 MB ROM image, release
$10F3, serves both boards — the machine-ID straps (§2.4) are the only identity input the ROM reads.

One platform-level note on naming: Apple's developer note calls the memory controller the **MCA** ("Memory
Controller and Arbiter") [1] p. 13; Apple's internal name for the same part, the one that survives in the part
number and in the machine-identification machinery, is **YMCA** (Apple part 343S1097). This page uses **YMCA**
for the chip and quotes the developer note's own prose as "MCA". Because the YMCA and the NuBus bridge (MUNI)
are parts of the family rather than devices with their own pages, this page documents them in full — the
YMCA in §2.3–§2.7, the MUNI in §5.4.

### 1.2 Membership and model variants

| Machine | Board | CPU / BClk | DSP CKI | Enclosure | NuBus slots | RAM max | VRAM |
|---|---|---|---|---|---|---|---|
| Macintosh Quadra 840AV | Cyclone (40 MHz build) | 68040 / 40 MHz | 66.6667 MHz | minitower, Quadra 800 form [1] p. 3 | three ($C/$D/$E) [1] p. 39 | 128 MB [1] p. 3 | 1 MB, expandable to 2 MB [1] p. 6 |
| Macintosh Centris 660AV (later Quadra 660AV) | Tempest (25 MHz build) | 68040 / 25 MHz | 55.5000 MHz | low-profile desktop, Centris 610 form [1] p. 3 | one short card ($C), via the adapter card that carries the MUNI [1] pp. 3, 39–40 | 68 MB [1] p. 3 | 1 MB, fixed [1] p. 6 |

The boot ROM carries a third and fourth variant as well: the shipped software set includes product records for
a 33 MHz Cyclone and a 33 MHz Tempest, alongside the two shipping grades [3] (*observed* in the ROM's machine
tables; see §6.3). The developer note documents only the two shipping models, and its identification table gives
SysEnvirons values 78 (Quadra 840AV processor, 40 MHz) and 60 (Centris 660AV processor, 25 MHz) [1] p. 8,
Table 1-1.

Everything else in this page is true of both boards. What belongs to one machine alone — the enclosure, slot
count, power budgets, the 660AV's processor-direct slot — is indexed in §6 and lives on the machine pages
([Quadra 840AV](q840av.md), [Centris 660AV](q660av.md)).

### 1.3 What every board carries

The common platform, from the developer note's chip inventory [1] pp. 13–17:

- **YMCA (MCA)** — memory controller and CPU-bus arbiter, eight DRAM banks, ROM control, bus timeouts [1] p. 13 (§2.3)
- **PSC** — nine DMA channels, I/O address decode, all system interrupts, the VIA1 and VIA2 functions [1] p. 13 ([PSC](psc.md))
- **MUNI** — the NuBus '90 bridge with block transfers and 20 MHz card-to-card bursts [1] p. 14 (§5.4)
- **DSP3210** — the real-time signal processor, a CPU-bus master [1] p. 13 ([DSP board](dsp3210-board.md))
- **CIVIC** — video/graphics frame-buffer controller, 1–2 MB VRAM, NTSC/PAL timing, VBL and video-in interrupts [1] p. 14 ([CIVIC](civic.md))
- **Sebastian** — dual-context color palette DAC with digital video/graphics mixing [1] p. 14 ([CIVIC](civic.md) §2.6)
- **VDC (Philips SAA7186)** — the video-in scaler [1] p. 15 ([VDC](vdc.md))
- **Mickey** — NTSC/PAL composite encoder fed from Sebastian [1] p. 15 (§2.7, [CIVIC](civic.md) §4.6)
- **Endeavor / Clifton Plus** — programmable pixel-clock synthesizer; Endeavor on the 840AV, Clifton Plus (or its
  second-generation sibling, distinguished at run time) on the 660AV [1] p. 17 ([CIVIC](civic.md) §1.4)
- **Digital Multistandard Decoder (Philips SAA7191B)** — the video-in decoder [1] p. 17 ([VDC](vdc.md) §2.3)
- **Curio** — SCSI (53C96-class), SCC (Z85C30-class) and MACE Ethernet macrocells in one ASIC [1] p. 16
  ([53C96](../../hardware/scsi/ncr-53c96.md), [SCC](../../hardware/scc.md), [MACE](mace.md))
- **New Age** — SuperDrive floppy controller, "based upon Industry Standard 765", GCR and MFM [1] p. 5 ([New Age](new-age.md))
- **Singer** — 16-bit stereo sound codec, serially attached to the PSC's sound DMA engine [1] p. 16 ([Singer](singer.md))
- **ATECS** — the telecom clock synchronizer for the GeoPort path [1] p. 16 (§5.5)
- **Cuda** — the system-management microcontroller: power, reset, PRAM, real-time clock, ADB [1] p. 16 ([Cuda](cuda.md))
- a 2 MB boot ROM shared byte-for-byte by both machines [1] p. 5 (§2.9)

### 1.4 What no board carries

The absences are as much a part of the platform definition as the additions, and the shipped software proves
them: the ROM's machine tables give every AV variant a decoder record whose slots for the classic Macintosh
I/O parts — ASC (sound), IWM/SWIM (floppy), a discrete VIA2, SONIC (Ethernet), the IOPs, parity support — are
all zero [3] (*observed* in the ROM's per-machine base-address table). Concretely:

- **No discrete VIA2.** The VIA2 function is a three-register window inside the PSC ([PSC](psc.md) §2.8); the
  machines have no 6522 there. VIA1 is likewise a register window inside the PSC at $50F00000 ([PSC](psc.md) §2.8).
- **No ASC.** There is no Apple Sound Chip; 16-bit sound is the Singer codec plus the PSC's dedicated sound DMA
  engine plus the DSP ([Singer](singer.md), [PSC](psc.md) §3.5).
- **No SWIM.** Floppy control is New Age, a 765-class controller with its own DMA channel [1] p. 5
  ([New Age](new-age.md)).
- **No parity.** "Both models ignore the parity connection on DRAM SIMMs" [1] p. 13.
- **No bus snooping.** "Neither model uses the optional bus-snooping feature of the MC68040 design" [1] p. 12 —
  the reason the platform's cache-coherency rules fall on software (§5.2).

---

## 2. Board architecture common to the family

### 2.1 One board, two speeds

"All the Macintosh Quadra 840AV and Macintosh Centris 660AV circuitry, except for the power supply, is contained
on a single multilayer circuit board" [1] p. 12, and the two products differ mainly "in physical form, speed,
and expansion facilities" [1] p. 3. The speed differences are strap-selected, not layout differences: the same
silicon runs at BClk 40.0000 MHz (840AV) or 25.0000 MHz (660AV), and the boot ROM programs the memory system's
timing latches and the MUNI's clock grade from the machine identity alone (§2.5). Even the DRAM speed grade is a
per-model rule rather than a different board: "The Macintosh Quadra 840AV requires 60 ns DRAM chips; the
Macintosh Centris 660AV can use 70 ns DRAM chips" [1] p. 6.

### 2.2 The signal buses

The platform's five internal buses [1] p. 18:

| Bus | Width | Members |
|---|---|---|
| CPU bus | 32 address, 32 data | the 68040, YMCA, PSC, MUNI, DSP3210, CIVIC |
| NuBus | 32 multiplexed address/data lines | MUNI and the slots ([NuBus](../../hardware/nubus/nubus.md)) |
| DAV connector | 16 video data lines, 5 audio lines | the video-in chain, the Singer serial bus, one slot position (§2.11) |
| video bus | two sets of 32 lines, or one set of 64 | VRAM, CIVIC, Sebastian, the VDC ([CIVIC](civic.md) §3.5) |
| I/O bus | 5 address, 16 data | the PSC and everything behind it ([PSC](psc.md) §3.3) |

The CPU bus "can read from and write to RAM, as well as read from ROM, in either single-address accesses or
four-address bursts" [1] p. 19 — a 68040 cache-line burst — and the burst-write protocol has an alternate form
for the non-processor masters, selected by the 040INPROGRESS line on the 660AV's PDS connector [1] p. 51.

### 2.3 The YMCA: memory controller and arbiter

The YMCA — the "MCA" of the developer note, "a complementary metal-oxide semiconductor (CMOS) chip in a
160-pin package" [1] p. 13 — is the platform's load-bearing ASIC. Apple's function list [1] p. 13:

- it "supports the bus interface between RAM and ROM memory and the main processor, DSP, MUNI, and PSC"
- it "controls access to eight banks of DRAM with up to 16 MB capacity per bank"
- it "controls access to ROM"
- it "performs arbitration for control of the CPU bus between the main processor, PSC, MUNI, and DSP"
- it "furnishes bus timeout signals for the main processor, I/O, MUNI, and DSP buses"

It is also where the machine stores its own identity: the CPU-ID register and the four machine-ID strap
registers live in its windows (§2.4), and the boot ROM's first hardware-dependent decision — which product
record to install — is a read of YMCA registers.

The YMCA's register file sits at **$50F30400**. Its unusual convention, visible in every access the ROM makes:
**every register is one bit wide, accessed as a longword with the value in bit 31** — which is why the ROM writes
only 0 or $FFFFFFFF (all-ones, bit 31 set) to the file [3] (*observed*; the writes are `MOVE.L #0,…` and
`MOVE.L #-1,…` throughout the speed-programming and sizing paths). The full file:

| Offset | Register | Function | Written by shipped software? |
|---|---|---|---|
| +$00 | DRAMspeed0 | DRAM timing strap 0 | yes, once per boot (§2.5) |
| +$04 | DRAMspeed1 | DRAM timing strap 1 | yes |
| +$08 | CPUspeed0 | CPU-bus timing strap 0 | yes |
| +$0C | CPUspeed1 | CPU-bus timing strap 1 | yes |
| +$10 | ROMspeed0 | ROM timing strap 0 | yes |
| +$14 | ROMspeed1 | ROM timing strap 1 | yes |
| +$18 | ROMspeed2 | ROM timing strap 2 | yes |
| +$1C | DSPspeed | DSP-bus timing | never [3] |
| +$20 | DRAMwidth0 | bank-pair 0/2 row/column mode | yes, during sizing (§2.6) |
| +$24 | DRAMwidth1 | bank-pair 1/3 | yes |
| +$28 | DRAMwidth2 | bank-pair 4/6 | yes |
| +$2C | DRAMwidth3 | bank-pair 5/7 | yes |
| +$30 | EPROMmode | EPROM/ROM mapping mode | never [3] |
| +$34 | 040Mode | 68040-specific mode | never [3] |
| +$38 | CPUID0 | machine-ID strap, bit 0 | read-only (§2.4) |
| +$3C | CPUID1 | machine-ID strap, bit 1 | read-only |
| +$40 | CPUID2 | machine-ID strap, bit 2 | read-only |
| +$44 | CPUID3 | machine-ID strap, bit 3 | read-only |
| +$48 | ClockSelect | video reference clock select (§2.7) | yes, on video mode changes |
| +$4C | Bypass | RGB/composite output route (§2.7) | yes, on video mode changes |
| +$50 + bank×$28 | per-bank block | bank `n` = 0…7: seven boundary bits then three size bits, one longword per bit | yes, during sizing (§2.6) |
| +$190 | Test_Mode | test mode | never [3] |
| +$194 | Refresh_Test | refresh test | never [3] |

The register names above are the conventional ones for this file; the addresses, the one-bit/longword convention
and the written/never-written split are established by the ROM's own accesses [3] (*observed* across the whole
image — a scan of every reference to $50F30400-class addresses finds no other offset touched). What the
never-written latches do is §7's first open question.

### 2.4 The YMCA: machine identification

The platform has two independent identity mechanisms, both read by the boot ROM within the first hundred
instructions of start-up:

**The CPU-ID register at $5FFFFFFC.** A longword read-only register whose value on every AV variant is
**$A55A2830** [3] (*observed*; the constant appears in the ROM's per-variant product records). The ROM's reader
validates three things, in order [3] ($4080FDA0): the top word reads $A55A (the platform-wide signature); the
location is *not writable* — the code writes a value and requires it to read back unchanged; and the access
completes without a bus error. The low half of the value encodes, in the classic Macintosh format, the processor
class (68040-class, high-end) and a flag stating that a supplemental identity register lives in the memory
controller — the pointer to the second mechanism.

**The four strap registers.** Each of CPUID0–CPUID3 (+$38…+$44) holds one strap bit in bit 31; the ROM reads
all four as longwords and assembles them into a nibble, CPUID3 as the most significant bit [3]
($4080F6EC–$4080F70E). The four board
variants and their straps:

| Board | CPUID3…CPUID0 | Nibble | Gestalt machine ID | Box flag |
|---|---|---|---|---|
| Cyclone 33 (unshipped) | 0 1 1 1 | $7 | 43 | 37 |
| Cyclone 40 — **Macintosh Quadra 840AV** | 1 1 1 1 | $F | 78 | 72 |
| Tempest 25 — **Macintosh Centris 660AV** | 1 0 1 1 | $B | 60 | 54 |
| Tempest 33 (unshipped) | 1 0 0 0 | $8 | 79 | 73 |

The nibble values and box flags are read from the ROM's product-record table [3] (*observed* in the image's
data); the Gestalt values for the two shipping models are documented by Apple [1] Table 1-1 p. 8, and the box
flags are confirmed from the system-software side: the System 7.1 enabler for these machines gates its entire
patch set on exactly box flags 72 and 54 [4]. On a strap mismatch the start-up code has no recovery path: it
spins forever in a two-instruction loop [3] ($4080F762, *observed* in the disassembly) — a machine that
presents an unmated strap nibble never boots.

### 2.5 The YMCA: speed and DRAM programming

Once the product record is chosen, and before any power-on self-test runs, the start-up code programs the YMCA's
speed latches and one register of the MUNI. The programming is a fixed per-variant pattern — the whole content
of "speed programming" on this platform is four longword writes and one MUNI write [3]
($4080F3B2–$4080F49F, *observed*; see §5.4 for the MUNI side):

| Variant | DRAMspeed (0, 1) | CPUspeed (0, 1) | ROMspeed (0, 1, 2) | MUNI Control |
|---|---|---|---|---|
| Cyclone 40 — Quadra 840AV | 0, set | 0, set | 0, 0, set | $1C |
| Cyclone 33 / Tempest 33 | set, 0 | set, 0 | set, set, 0 | $18 |
| Tempest 25 — Centris 660AV | 0, 0 | 0, 0 | 0, set, 0 | $14 |

("set" = the register written with all-ones, i.e. bit 31 = 1.) The patterns correspond to the documented timing:
the 840AV runs 60 ns DRAM at a 40 MHz bus clock, the 660AV 70 ns DRAM at 25 MHz [1] p. 6; and ROM access costs
seven bus clock cycles on the 840AV and five on the 660AV [1] p. 20. What the individual bits program inside the
YMCA — CAS latency, wait states, burst length — is nowhere stated (§7); the ROM knows three fixed patterns for
three clock grades and nothing else. The dispatch is guarded by the decoder kind: a machine identified by some
other controller skips the writes entirely [3].

### 2.6 The YMCA: RAM sizing and bank decode

The YMCA drives eight DRAM banks. Its per-bank register block (§2.3) holds, per bank, **seven boundary bits**
(address bits A20–A26 — a 16 MB bank granularity) and **three size bits**, each bit in its own longword; the
blocks are 40 ($28) bytes apart, bank 0 at +$50. The shipped sizing algorithm, reconstructed instruction by
instruction from the ROM [3] (the sizing path at $4080ADAF and the width-write sequence near it, *observed*):

1. **Zero all four DRAMwidth registers**, forcing every bank pair into standard (equal rows and columns) mode —
   deliberately, because diagnostics may ask for a second sizing pass.
2. For each of the eight banks, serially shift out the seven boundary bits (banks sit at 16 MB spacing) and the
   three size bits, one `MOVE.L` per bit, and test the bank's actual extent.
3. **Size every bank a second time in wide mode** — all DRAMwidth registers set, which presents more rows than
   columns — and keep whichever pass reported the larger capacity, programming the width registers per bank
   pair (0/2, 1/3, 4/6, 5/7).
4. Merge the boundary registers so that contiguous banks concatenate into one logical range; the written size
   codes are capped at 16 MB per bank.

The limits fall out of the algorithm plus the developer note: the minimum bank is 1 MB, a bank may hold at most
16 MB, and the platform maximum is therefore 128 MB across eight banks [1] p. 3 — the 660AV's 68 MB ceiling is
a configuration limit (4 MB soldered plus two SIMM slots [1] p. 13), not a controller one. There is **no
interleave register anywhere in the file**: nothing in the shipped software writes an interleave control, so if
the 840AV's paired-bank interleaving exists it is automatic in hardware, invisible to software (*inferred —
unverified*, §7). There is likewise no parity: the parity pin is ignored [1] p. 13 and no parity-status register
is ever read [3].

### 2.7 The YMCA: the video routing bits

Two YMCA registers belong to the video subsystem and are written by the CIVIC driver rather than by start-up:

- **ClockSelect (+$48)** — a longword write of 0 or all-ones selecting which reference oscillator feeds the
  pixel-clock synthesizer: 0 for the 14.31818 MHz NTSC-family reference, all-ones for the 17.734475 MHz PAL
  reference [3] (*observed* in the video driver's mode-switch path; the two oscillator frequencies and their
  consumers are [1] Table 2-2 p. 18 — "Sebastian, Mickey"). The write always precedes the synthesizer
  programming ([CIVIC](civic.md) §1.4, §4.2).
- **Bypass (+$4C)** — a longword write of 0 (RGB to the DB-15 monitor port) or all-ones (route the pixel stream
  to Mickey for composite/S-video encoding). This single bit is, with CIVIC's sync-output gate, the entirety of
  the software's control over the television output path ([CIVIC](civic.md) §4.6; §7).

### 2.8 DRAM modules and rules

DRAM lives on 72-pin SIMMs, "one or two banks of DRAM with up to 16 MB of capacity per bank" [1] p. 13. The
geometry the YMCA's sizing supports [1] p. 19:

| Bank size (MB) | Organization | Row address bits | Column address bits |
|---|---|---|---|
| 1 | 256 Kbit × 4 | 9 | 9 |
| 1 | 1 Mbit × 4 | 10 | 10 |
| 2 | 512 Kbit × 8 | 10 | 9 |
| 4 | 4 Mbit × 4 | 11 | 11 |
| 8 | 2 Mbit × 8 | 11 | 10 |
| 4 | 4 Mbit × 4 | 12 | 10 |
| 8 | 2 Mbit × 8 | 12 | 9 |
| 16 | 1 Mb × 16 | 11 | 9 |

Module rules from the same source: "DRAM compatible with either model must accept the column address strobe
(CAS) before the row address strobe (RAS) for refresh and may not have more than eight chips per bank" [1] p. 13
(composite SIMMs cannot be used [1] p. 52); the parity connection is ignored [1] p. 13; access time 60 ns max
(840AV) or 70 ns (660AV) [1] p. 6.

### 2.9 The boot ROM

Both machines carry "identical ROM chips totaling 2 MB", and the chips "contain some of the system software that
is on the hard disk in other Apple computers" [1] p. 5 — the Real Time Manager, the DSP operating system and
driver, the serial DMA driver and its HAL, the video digitizer component, the SCSI Manager 4.3 library, and the
floppy driver all live in ROM, with the System Enabler supplying newer builds of some of them [4]. The chips are
120 ns parts [1] p. 20.

The image (release $10F3, ROM version word $077D, image checksum $5BF10FD1) maps at the 32-bit base
**$40800000**, so a CPU address is `$40800000 + file offset` [3]. Its container layout is the classic Macintosh
ROM header: the checksum longword at file offset $00 doubles as the initial supervisor stack pointer the 68040
fetches from address 0 at reset, and the longword at offset $04 — the value $0000002A — is the initial program
counter, pointing at a `JMP` into the ROM's start-up entry [3] (*observed*; the header, its fields and the
cold-start path are decoded in the disassembly). The image's second megabyte also appears, whole, in the 24-bit
compatibility map at $900000–$9FFFFF (§3.4) [3]. A full resource-level description of the image is the
[Mac ROM format](../../formats/mac-rom.md) page's subject; what matters at board level is:

- **One image serves both machines and all four speed variants** — the product records for Cyclone 33/40 and
  Tempest 25/33 are all in the one table [3] (§2.4), and the four per-variant Ethernet configuration resources
  the ROM carries are byte-identical, evidence that no device-visible difference exists between speed grades
  [3] (*observed* at file offsets $1AEE30, $1AEE90, $1AEEF0, $1AEF50).
- **The ROM is not just code**: the CIVIC video timing tables, the declaration-ROM block for the built-in video
  pseudo-slot, the boot and error chimes (16-bit stereo PCM, 24 kHz, in the image at file offset $C5D24), and
  the DSP3210 boot segments are all ROM data [3] (*observed*; the declaration data carries the June 25, 1993
  build date).

ROM control — wait states, access timing — is the YMCA's ROMspeed latches' business (§2.5); the mapping is §3's.

### 2.10 VRAM

Video memory is separate from main memory: "Both models store video information in RGB form in a video frame
buffer separate from main memory" [1] p. 5, controlled by CIVIC. The platform ships "two banks of VRAM soldered
in, each bank providing 0.5 MB of storage" [1] p. 33 — 1 MB total — and the Quadra 840AV can expand to 2 MB with
two more banks on 68-pin SIMMs, using 80-ns chips [1] pp. 33–34, 54; the Centris 660AV cannot expand [1] p. 6.
The two-bank structure is functional, not just physical: one bank can drive the graphics image while the other
carries live video to be mixed by Sebastian ([CIVIC](civic.md) §1.3, §3.5). VRAM sits at $50100000 in the 32-bit
map, deliberately with no 24-bit alias (§3.4). Sizing, modes and the per-machine color-depth limits are
[CIVIC](civic.md)'s and the machine pages' subject (§6).

### 2.11 The DAV expansion connector

The digital audio/video (DAV) connector is a 40-pin header that gives one NuBus card position direct access to
"the system's 4:2:2 unscaled YUV video input signal and to the digital audio signal input for the Singer codec"
[1] p. 42 — video on the CCIR 601 format the compression chips of the day consumed, and audio as additional
subframes on the Singer time-division-multiplexed serial bus [1] pp. 44–45 ([Singer](singer.md)). On the Quadra
840AV the connector is on the main board "in line with NuBus slot address $C (the slot nearest the center of the
computer)"; on the Centris 660AV it is on the optional NuBus adapter card [1] p. 42. A card on the DAV connector
may also *source* video: it "may disable the DMSD and feed its own YUV video to the VDC" [1] p. 32
([VDC](vdc.md) §1.1). The connector's pin assignments, signal timing and 20 pF load limit are [1] pp. 42–45.

---

## 3. Memory map & address decode shared by the family

### 3.1 The 32-bit physical map

The developer note gives region-level boundaries only (the bus-timeout split of §5.2 is its one address-space
statement [1] p. 19); the working map below is the one the boot ROM itself installs, read from its 68040 page
templates and physical-address tables [3] (*observed* in the ROM's MMU initialization data):

| Physical range | Contents | Mapping attributes |
|---|---|---|
| $00000000 – top of RAM | DRAM, eight banks, contiguously merged by the YMCA (§2.6) | cacheable |
| $40800000–$409FFFFF | the 2 MB boot ROM (§2.9); second megabyte also readable at $900000 in 24-bit mode | cacheable |
| $50036000–$500FFFFF | the video I/O region: CIVIC and the video-in control it hosts | non-serialized (§3.2) |
| $50100000–$502FFFFF | VRAM (§2.10) and the video-in capture buffer ([VDC](vdc.md) §1.5) | non-serialized |
| $50F00000–$50F3FFFF | the I/O island — every device register window in the machine (§3.2) | serialized |
| $50F40000–$50F7FFFF | the I/O island's alias — same devices, non-serialized | non-serialized |
| $5FFFFFFC | the CPU-ID register (§2.4) | read-only, longword |
| $70000000–$7FFFFFFF | the E-Disk — the RAM-disk aperture the ROM reserves | — |
| $A0000000–$EFFFFFFF | super slot space, slots $A–$E, 256 MB each [5] | transparent, non-cacheable |
| $F9000000–$F9FFFFFF | standard slot space of slot $9 — the built-in video's software slot alias | transparent (TT0) |
| $FA000000–$FEFFFFFF | standard slot space, slots $A–$E, 16 MB each [5] | transparent, non-cacheable |
| rest of $80000000–$FFFFFFFF | NuBus and slot apertures, transparent | non-cacheable, serialized (TT1) |

Two transparent-translation registers cover the whole non-RAM world for the 68040: TT1 spans
$80000000–$FFFFFFFF (the NuBus aperture), and TT0 spans the slot-9 standard window [3]. During start-up,
before the MMU is configured at all, the ROM installs two *data* transparent translations — one covering the
whole $5xxxxxxx I/O region and one covering NuBus — so that POST can reach the CPU-ID register and the I/O
island with the MMU off [3] (*observed* in the cold-start path: `DTT0`/`DTT1` are programmed within the first
instructions of the start-up entry).

### 3.2 The I/O island and its alias

Every register in the machine except CIVIC's, the CPU-ID register and the VRAM lives in one 256 KB window,
**$50F00000–$50F3FFFF** — the "I/O island". A second window, **$50F40000–$50F7FFFF**, decodes to the same
devices [3] (*observed*: the ROM's own MMU template carries both, the alias explicitly labeled non-serialized).
The distinction is cache discipline: the serialized window is the one software is expected to use for
accesses whose ordering and completion matter; the non-serialized alias serves the bulk paths. The video I/O
region at $50036000 is a third spelling of the same idea — the ROM's decoder record names CIVIC at $50F36000
while every access the ROM makes uses $50036000, and byte-pattern scans of the whole image find the $50F36000
form referenced nowhere [3] (*observed* by scan; [CIVIC](civic.md) §2.1 carries the finding). The two forms
alias — the decode reaches the same register file through both — but the exact alias rule, which address bits
the decoder ignores, is not established (§7). The pattern has Macintosh precedent: the RBV machines' register
file answered at both $5001xxxx and $50F1xxxx-class spellings [2].

### 3.3 Device bases and how software finds them

The fixed bases, as the ROM's per-family decoder record lists them [3] (*observed* in the ROM's machine tables;
each device page carries its own):

| Base | Device | Page |
|---|---|---|
| $50F00000 | the VIA1 window (Cuda transport, timers, 60 Hz) | [PSC](psc.md) §2.8, [Cuda](cuda.md) §2.1 |
| $50F02000 | the VIA2 window (slot, SCSI, floppy, sound-frame interrupts) | [PSC](psc.md) §2.8 |
| $50F04000 | the SCC (Curio's serial macrocell, both channels) | [SCC](../../hardware/scc.md) |
| $50F08000 | the Ethernet station-address PROM | [MACE](mace.md) §4.3 |
| $50F18000 | the 53C96-class SCSI controller (Curio) | [53C96](../../hardware/scsi/ncr-53c96.md) |
| $50F1C000 | the MACE Ethernet controller (Curio) | [MACE](mace.md) §1.4 |
| $50F2A000 | the New Age floppy controller | [New Age](new-age.md) |
| $50F2E000 | the pixel-clock synthesizer (Endeavor / Clifton Plus) | [CIVIC](civic.md) §1.4 |
| $50F30000 | the MUNI | §5.4 |
| $50F30400 | the YMCA | §2.3 |
| $50F30800 | Sebastian (CLUT/DAC) | [CIVIC](civic.md) §2.6 |
| $50F31000 | the PSC — channels, level registers, sound/DSP block | [PSC](psc.md) §2 |
| $50036000 (≡ $50F36000) | CIVIC | [CIVIC](civic.md) §2.1 |

None of these constants is hard-coded by well-behaved software: every driver recovers its base at run time from
the machine's decoder-information record, reached through the low-memory pointer at $0DD8 — the idiom is
documented once, at [PSC](psc.md) §2.1, and holds for every device in the table. The record also carries a
validity bit per base, which start-up tests before first use; the 660AV's record deliberately omits the
expansion-feature bits so that the ROM probes the MUNI at run time instead of trusting the table — a 660AV may
have no NuBus adapter at all [3] (*observed* in the per-variant records; the probe is §5.4). The ROM itself
breaks its own rule where convenient: the floppy and DSP drivers hard-code their PSC channel aliases, and the
MUNI's control register is written by absolute address [3].

### 3.4 The 24-bit compatibility map

For 24-bit-only software the ROM installs a compatibility mapping [3] (*observed* in the ROM's 24-bit page
template):

| 24-bit range | 32-bit destination |
|---|---|
| $000000–$7FFFFF | RAM |
| $800000–$8FFFFF | the ROM's first megabyte |
| $900000–$9FFFFF | the ROM's **second** megabyte — the standard-slot-9 window, repurposed |
| $A00000–$EFFFFF | standard slot space, slots $A–$E ($FA00000–$FEFFFFF) |
| $F00000–$F03FFF | the I/O island, $50F00000 (serialized form) |
| $F40000–$F43FFF | the I/O island alias, $50F40000 (non-serialized form) |

There is deliberately **no 24-bit alias of VRAM** — the video records set the logical-24 VRAM base to zero [3]
(*observed*; [CIVIC](civic.md) §2.1) — so all video memory access is 32-bit-mode. The slot-9 repurposing means
a 24-bit-mode access to what looks like a NuBus slot reads ROM; software that walks slot space in 24-bit mode
will find no cards, which is the point — the slot population is 32-bit clean by construction.

### 3.5 The startup overlay

At power-on the YMCA maps the ROM over the bottom of the address space: "At the beginning of the system startup
process, the MCA maps the ROM code to the lower end of the RAM address space, starting at address $0000 0000.
When the main processor is reset, it sets the program counter to the 32-bit address found at $0000 0004, which
transfers execution to the actual system software entry point in the space $4000 0000 to $4FFF FFFF. After this
first access, ROM is no longer mapped to the bottom of the RAM space" [1] p. 20. So the reset picture is: the
initial stack pointer comes from the ROM's checksum longword (address 0 while overlaid), the initial PC from the
longword at address 4, and both are ROM contents [3] (§2.9). No software action anywhere in the ROM clears the
overlay — not an address write, not a VIA bit (the classic Macintosh `vOverlay` line does not exist on this
platform; the start-up code re-derives its position from the ROM base instead) — so the overlay's removal is
done by the YMCA itself, on a trigger the documentation names only as "this first access" [1] p. 20. Whether
that means the first fetch from the entry point, the first access to the $40800000 window, or the first RAM
write is not determined by the evidence (§7).

### 3.6 Undecoded space and the error contract

Two levels of failure exist for an address nothing answers:

- **Bus timeout, not recoverable.** The YMCA "furnishes bus timeout signals" [1] p. 13: a cycle to which no
  master responds within the critical time terminates with a bus error — 32 µs for the NuBus address space
  ($60000000–$FFFFFFFF), 16 µs elsewhere, and 16 DSP clocks if the DSP fails to start a cycle after being
  granted the bus [1] p. 19 (§5.2). The MUNI adds its own 25.6 µs NuBus-transaction timeout [1] pp. 19, 39.
- **The slot probe contract.** Empty slots and absent adapters must fail *cleanly*: the start-up code probes
  the MUNI under a bus-error handler on a 660AV and records the bridge as absent when the access faults [3]
  (*observed*), and the Slot Manager's declaration-ROM search treats a faulting slot read as "no card" [5]
  ([declaration ROM](../../hardware/nubus/declaration-rom.md)). An address in slot space that returns garbage
  instead of faulting turns every empty slot into a broken card.

What an access to an undecoded offset *inside* the I/O island returns — bus error, or a floating value — is
not established by any shipped software: the only deliberate in-island fault on these machines is the 660AV's
absent MUNI [3] (§7).

---

## 4. Device roster

The complete part list of the family, grouped by role. Each device's registers, behaviour and programming model
are its own page's subject; this roster states only what the part is and where it lives.

### 4.1 The controller core

| Part | Apple part / package | Role | Where |
|---|---|---|---|
| YMCA (MCA) | 343S1097, 160-pin CMOS [1] p. 13 | memory controller, eight-bank DRAM/ROM control, CPU-bus arbiter, bus timeouts, machine identity | $50F30400 — §2.3 |
| PSC | 343S1100, 208-pin CMOS [1] p. 13 | nine DMA channels, I/O decode, all interrupts, VIA1/VIA2 functions, sound DMA | $50F31000 (+ the two VIA windows) — [PSC](psc.md) |
| MUNI | 343S1039, 208-pin CMOS [1] p. 14 | the NuBus '90 bridge | $50F30000 — §5.4 |
| DSP3210 | AT&T 32-bit floating-point DSP, 8 KB internal RAM [1] p. 13 | real-time processing, a CPU-bus master | CPU bus; host registers inside the PSC window — [DSP board](dsp3210-board.md) |
| Cuda | microcontroller, 32.768 kHz clock [1] pp. 16–17 | power, reset, PRAM, real-time clock, ADB, GeoPort power signaling | via the VIA1 window, $50F00000 — [Cuda](cuda.md) |

### 4.2 The I/O side

| Part | Role | Where |
|---|---|---|
| Curio — 53C96-class SCSI core | the internal/external SCSI bus, NCR 53C94/96 register model, DMA'd by PSC channel 0 | $50F18000 — [53C96](../../hardware/scsi/ncr-53c96.md) |
| Curio — SCC core | both serial ports (Zilog Z85C30 register model, 8-byte FIFOs each way [1] p. 16), LocalTalk on the printer port, GeoPort on the modem port | $50F04000 — [SCC](../../hardware/scc.md) |
| Curio — MACE core | Ethernet (Am79C940), DMA'd by PSC channels 1/2, station address from a PROM at $50F08000 | $50F1C000 — [MACE](mace.md) |
| New Age | the SuperDrive floppy controller, "based upon Industry Standard 765", GCR and DOS-format MFM, disk-insertion interrupt, DMA'd by PSC channel 3 | $50F2A000 — [New Age](new-age.md) |

The serial, SCSI and Ethernet functions are three macrocells of the one Curio ASIC, but no shipped software
addresses Curio as a unit: each core has its own base, its own decoder slot and its own driver [1] p. 16;
[3]. The LocalTalk patch chip on the printer port and the GeoPort hardware on the modem port are port-level
features of the same serial subsystem [1] p. 22.

### 4.3 Video and graphics

| Part | Role | Where |
|---|---|---|
| CIVIC | frame-buffer controller for 1–2 MB VRAM, convolver, NTSC/PAL timing generator, VBL and video-in interrupts; also the capture-buffer controller | $50036000 — [CIVIC](civic.md) |
| Sebastian | dual-context color palette and DAC; mixes video and graphics streams, each with its own lookup table; alpha-blended overlay | $50F30800 — [CIVIC](civic.md) §2.6 |
| VDC (Philips SAA7186) | the video-in scaler: YUV 4:2:2 in, 8-bit gray/15-bit RGB/16-bit YUV out, horizontal and vertical filtering, writes the capture buffer in VRAM | no address of its own — an I²C slave at $B8 reached through the Cuda — [VDC](vdc.md) §2.1 |
| DMSD (Philips SAA7191B) | the video-in decoder: NTSC/PAL/SECAM composite or S-video to CCIR 601 YUV | I²C slave at $8A, same bus — [VDC](vdc.md) §2.3 |
| Mickey | the NTSC/PAL composite and S-video output encoder, fed from Sebastian's DACs | no register surface; routed by the YMCA Bypass bit (§2.7) — [CIVIC](civic.md) §4.6 |
| Endeavor / Clifton Plus | the programmable pixel-clock synthesizer feeding Sebastian; Endeavor on the 840AV, Clifton Plus or its second-generation sibling (probed at driver-open) on the 660AV | $50F2E000 — [CIVIC](civic.md) §1.4 |
| TDA8708/TDA8709 | the video-input ADC pair ahead of the DMSD | analog front end, no software surface [1] p. 32 |

The developer note's own parts list labels the video-in scaler "Phillips 7169"; the part is the SAA7186, and the
note's label is one of its known slips — the identification and the register map are [VDC](vdc.md) §1–§2.

### 4.4 Audio and telecom

| Part | Role | Where |
|---|---|---|
| Singer | the 16-bit stereo codec ("I/O chip that constitutes a 16-bit digital sound codec", conforming to the IT&T ASCO-2300 specification [1] p. 16); a four-wire serial frame bus on which subframe 1 of each 256-bit frame carries the system sound and the other subframes are free for DAV cards [1] p. 44 | serially attached to the PSC sound engine — [Singer](singer.md) §3.1 |
| ATECS | the Apple Telecom External Clock Synchronizer: locks the DSP and sound subsystems to an external clock received through GeoPort, or generates the 45.1584/49.152 MHz families for 44.1/48 kHz operation [1] p. 16 | no register surface in the ROM; its outputs are PSC and DSP clock inputs (§5.5) |
| DSP3210 (audio role) | all system sound synthesis/capture and the PlainTalk microphone path run through DSP tasks; there is no non-DSP sound path anywhere in the shipped software set | [DSP board](dsp3210-board.md) §3.7–§3.8, [Singer](singer.md) |

### 4.5 Absent devices

For completeness, the parts a Macintosh of this class might be expected to carry and these boards do not —
each provably absent from the ROM's decoder record for every AV variant [3] (*observed*; §1.4): ASC, IWM and
SWIM, a discrete VIA2 (6522), SONIC, the IOPs, the Orwell/djMEMC-class memory controllers, and parity support.
The SCSI core answers the 53C96 register model rather than the 5380 of the compact machines
([53C96](../../hardware/scsi/ncr-53c96.md)), and there is no real-time-clock chip separate from the Cuda
([Cuda](cuda.md) §2.5).

---

## 5. Interrupt, bus, and clock architecture

### 5.1 Interrupt levels

The PSC drives the 68040's three `/IPL` lines and thereby presents the whole machine — slots, video, DSP, every
peripheral — as the classic seven-level 68k autovector model. The developer note pins the electrical contract:
the lines are "interrupt priority lines from the PSC; not to be used as wire-OR lines; can be monitored by a PDS
card" [1] Table 3-22 p. 49. The family-wide level map:

| IPL | Sources |
|---|---|
| 1 | the VIA1 window ($50F00000): its two timers, CA1 (the 60 Hz-class retrace tick), the shift register and CB1/CB2 (the Cuda transport), and the one-second tick from Cuda [2] p. 145 |
| 2 | the VIA2 window ($50F02000): external SCSI, the slot aggregate (slots $C/$D/$E plus on-board video VBL), MUNI, the floppy interrupt, the sound frame interrupt |
| 3 | MACE Ethernet |
| 4 | Singer codec status, SCC port A, SCC port B, and any DMA channel completion (fanning out through the PSC's DMA status register) |
| 5 | the DSP (to host), sound/DSP frame overrun |
| 6 | the periodic 60 Hz-class timer, SCC port A, SCC port B |
| 7 | NMI (not PSC-sourced) |

The registers behind levels 2–6, the dispatchers' read-twice discipline, the enable conventions and the
deferred-level-2 mechanism are [PSC](psc.md)'s subject (§§2.2, 2.8, 3.1–3.2, 4.2) — this page does not restate
them. Two rules are family-level and belong here:

- **Level 2 is not optional.** Devices whose classic-Mac ancestors interrupted through VIA2 still do, through
  the pseudo-VIA2 window, while the devices new to this platform use the PSC's level registers; a driver that
  only knows the PSC register file misses the entire level-2 bank ([PSC](psc.md) §3.1).
- **Interrupt latency is bounded by convention.** Apple's compatibility contract for the platform is explicit:
  "Do not disable interrupts for longer than 0.5 ms" [1] p. 7 — the PSC's double-buffered channels and the
  serial FIFOs are sized around that budget, and the DMA channels' two register sets exist precisely to
  "increase the limit of system interrupt latency when the data input is continuous" [1] p. 29.

### 5.2 The CPU bus and its four masters

"Four chips are able to take control of the CPU bus: the main processor, the MUNI, the PSC, and the DSP (which
is asynchronous with respect to the main processor). The MCA performs arbitration between these chips for
control of the CPU bus" [1] p. 18. Nothing in Apple's documentation or in the shipped software states the
arbitration *policy* — priorities, parking, or worst-case grant latency; the only documented arbitration
figures are the timeout rules the YMCA also enforces [1] p. 19:

- a master that does not respond to a cycle-start signal within the critical time loses the bus and gets a bus
  error — 32 µs in NuBus space ($60000000–$FFFFFFFF), 16 µs everywhere else;
- the DSP, granted the bus, must issue a cycle start within **16 DSP clock cycles** or the YMCA terminates its
  control with a bus error — a timeout counted in the DSP's own clock domain, the arbiter running on CKI.

DRAM access costs, in BClk cycles (CKI for the DSP), are tabulated by the developer note [1] Table 2-3 p. 20 —
headline numbers: a single read costs the processor 6 cycles on the 840AV and 4 on the 660AV, a burst read
6-3-3-3 / 4-2-2-2, the PSC and MUNI 5 / 4 and 5-3-3-3 / 4-2-2-2, and the DSP 9 and 9-4-4-4; the full table
including write timings is [1] p. 20. The DRAM refresh cycle "takes eight main processor clock cycles, but does
not affect RAM access timing. DRAM refresh does not occur when the DSP controls the CPU bus" [1] p. 20 — the
last clause a hint that the DSP's bus tenure is expected to be short and burst-shaped.

Because there is no bus snooping ([1] p. 12; §1.4), cache coherency across DMA is software's problem, and the
platform says so where it matters: "in a virtual memory environment, software must guarantee that memory pages
are contiguous when DMA transfers controlled by the PSC cross a page boundary" [1] p. 29, and the PSC's
channels "can access RAM or ROM but cannot access the I/O or NuBus address spaces" [1] p. 29 — so no DMA path
can touch a device register. The boot ROM's own start-up sequence is cautiously ordered around the same
coherency question: the 68040's data cache is left disabled until after the slot subsystem is initialized
(*observed* in the start-up path's cache-enable ordering [3]); the shipped software never states why.

### 5.3 The I/O bus

Behind the PSC lies a separate 5-address-line, 16-data-line I/O bus [1] p. 18, and the PSC is its arbiter:
"the PSC grants its DMA channels two accesses for every one access granted to the main processor", with a
fixed channel priority order for both sides of the chip [1] pp. 29–30. The two priority tables (FDC first to
the I/O bus; SndOut first to the CPU bus, SCSI last on both) are transcribed at [PSC](psc.md) §3.3, together
with the observation that interrupt priority and bus priority are independent orderings on this platform.

### 5.4 NuBus and the MUNI

The NuBus implementation is the **Macintosh Universal NuBus Interface (MUNI)**, "a CMOS chip in a 208-pin
package" [1] p. 14, Apple part 343S1039. On the Quadra 840AV it is on the main board; on the Centris 660AV "it
is on an optional NuBus adapter card" [1] p. 14 — the card that also carries the machine's single NuBus
connector [1] p. 40. The MUNI's documented capabilities [1] p. 14:

- "supports the full range of NuBus master/slave transactions with single or block moves, including dumps and
  runs in which the main processor is master and NuBus is slave"
- "supports faster data transfer rates to and from the CPU bus"
- "supports NuBus '90 data transfers between cards at a clock rate of 20 MHz"
- "provides first-in, first-out (FIFO) buffering of data between the CPU bus and accessory cards"

The interface follows the NuBus '90 specification (ANSI/IEEE Std 1196-1990); slot geography is fixed: "Each of
the three Macintosh Quadra 840AV slots has a 4-bit geographic address. The addresses are $C, $D, and $E … The
Macintosh Centris 660AV slot is address $C" [1] p. 39. Slot space itself — standard ($Fs000000, 16 MB) and
super ($s0000000, 256 MB) windows, declaration ROMs, arbitration — is [NuBus](../../hardware/nubus/nubus.md)'s
and [Designing Cards and Drivers][5]'s subject; the MUNI-specific behaviors are:

**The register file, $50F30000** [3] (*observed*; only the first three registers are ever touched by shipped
software):

| Offset | Register | Shipped-software usage |
|---|---|---|
| +$00 | IntCntrl — interrupt control | written with 1 once per boot, to disable the MUNI's interrupt (no handler exists) |
| +$04 | IntStatus — interrupt status | never read |
| +$08 | Control — system/clock control | written with $1C / $18 / $14 by speed grade (§2.5); read once as the presence probe |
| +$0C | BlkAttmpt — per-slot block-attempt enables | written per slot by the block-transfer selector; bit `n` on a write is slot `n`+1 |
| +$10 | Status | never accessed |
| +$14 | Test | never accessed |

**Presence probing.** A 660AV may have no MUNI at all, so the start-up code reads Control under a bus-error
handler and clears the bridge's validity bit if the access faults [3] (*observed*; the 660AV's decoder record
deliberately omits the expansion bases to force this probe). A MUNI-less 660AV must therefore answer the read
of $50F30008 with a clean bus error, not with data [3].

**Block transfers.** "Block transfers from NuBus are always enabled, but block transfers to NuBus must be
enabled" — either by the card's declaration ROM declaring slave block transfers of size 4 (the only size the
MUNI generates [1] p. 411), or by the `_SlotBlockXferCtl` trap, which sets the slot's bit in BlkAttmpt [1]
pp. 411–412 [3]. The Slot Manager performs the ROM-driven half automatically after a card's `PrimaryInit` [1]
p. 411.

**FIFOs and concurrency.** The MUNI "provides separate FIFO buffers for data on the CPU bus and on NuBus" and
"these buffers can operate concurrently": the CPU-bus side holds 4 longwords of read data (one burst) and 16
of write data (four bursts), the NuBus side 16 read (one block-16) and 32 write (two block-16s) [1] p. 39,
Table 2-16.

**Timeouts.** "MUNI generates a bus error if any transaction takes longer than 25.6 µs" [1] pp. 19, 39 — on top
of the YMCA's 32 µs NuBus-space cycle timeout (§5.2).

**Interrupts.** The MUNI's interrupt line reaches the PSC-VIA2 window (§5.1, level 2), but shipped software
never enables or services it: the boot ROM's first act toward the bridge after the speed write is to disable
its interrupt outright [3] (*observed*). Which conditions the MUNI would raise is undocumented (§7). Slot
interrupts are not the MUNI's to carry: each slot's request line is collected in the PSC-VIA2 window ([PSC](psc.md)
§2.8), the platform's version of the classic slot-interrupt bank.

### 5.5 The clock tree

The platform's clocks, from Table 2-2 of the developer note [1] pp. 17–18 (asterisked values are the Centris
660AV's):

| Domain | Clock | Frequency (MHz) | Source → consumer |
|---|---|---|---|
| CPU bus | PClk | 80.0000 / 50.0000* | divider → main processor, MCA, CIVIC |
| CPU bus | BClk | 40.0000 / 25.0000* | divider → main processor, MCA, CIVIC, PSC, MUNI |
| DSP | CKI | 66.6667 / 55.5000* | oscillator → DSP, MCA |
| I/O | PClk/4 | 20.0000 / 12.5000* | oscillator → divider |
| I/O | C32M | 31.3344 | oscillator → PSC (its I/O-side synthesizer source) |
| I/O | C16M | 15.6672 | PSC → New Age, Curio |
| I/O | C22_5792M | 22.5792 | PSC → Singer (44.1 kHz family) |
| I/O | C24_576M | 24.5760 | PSC → Singer (48 kHz family) |
| I/O | C25M | 25.0000 | oscillator → SCSI |
| I/O | C20M | 20.0000 | crystal → MACE Ethernet |
| I/O | CudaClk | 0.032768 | crystal → Cuda |
| audio | C15_0528M | 15.0528 | crystal → ATECS (44.1 kHz) |
| audio | C16_384M | 16.3840 | crystal → ATECS (48 kHz) |
| audio | C45_1584M | 45.1584 | ATECS → PSC (44.1 kHz reference) |
| audio | C49_1520M | 49.1520 | ATECS → PSC (48 kHz reference) |
| NuBus | C40M | 40.0000 | oscillator → MUNI |
| NuBus | C20M | 20.0000 | MUNI → the slots (NuBus '90 double-speed clock) |
| NuBus | CN10M | 10.0000 | MUNI → the slots (the standard NuBus clock) |
| video | video in | 26.8000 | crystal → DMSD |
| video | dot clock | mode-dependent | Endeavor/Clifton Plus → Sebastian |
| video | NTSC | 14.31818 | oscillator → Sebastian, Mickey |
| video | PAL | 17.734475 | oscillator → Sebastian, Mickey |

The family-level facts that fall out of the table:

- **BClk is the platform's metronome.** DRAM cycle times, ROM accesses and the PSC's and MUNI's CPU-bus side
  all count in BClk [1] p. 20; the DSP alone counts in CKI.
- **The audio clocks are generated, not selected.** Software never supplies a sample-rate clock: the ATECS
  synthesizes the 44.1/48 kHz families, the PSC distributes the matching codec clocks, and the rate choice is a
  register bit ([PSC](psc.md) §1.4, [Singer](singer.md)).
- **The pixel clock is programmed per mode** through the synthesizer at $50F2E000, after the YMCA's ClockSelect
  bit picks the NTSC- or PAL-family reference ([CIVIC](civic.md) §1.4).
- **The DSP is asynchronous** to the CPU bus by design ([DSP board](dsp3210-board.md) §1.3), which is why the
  YMCA's DSP grant timeout is counted in CKI and why CKI is listed as feeding the MCA as well [1] Table 2-2
  p. 18.

---

## 6. Per-machine index

### 6.1 Macintosh Quadra 840AV — [q840av.md](q840av.md)

The shipping 40 MHz Cyclone. Identity: strap nibble $F (§2.4), Gestalt/SysEnvirons machine ID 78 [1] p. 8, box
flag 72 [4]. The minitower machine in the Quadra 800 enclosure, with three NuBus slots at geographic addresses
$C/$D/$E [1] pp. 3, 39, the DAV connector in line with slot $C [1] p. 42, and space for up to three internal
SCSI devices [1] p. 3. Everything speed- or capacity-related on this machine is the high-grade value: BClk
40.0000 MHz, CKI 66.6667 MHz (§5.5), 60 ns DRAM mandatory [1] p. 6, ROMspeed/MUNI patterns of §2.5's first row.
It is the machine of the two that supports VRAM expansion (1 MB to 2 MB, 80 ns SIMMs) [1] pp. 6, 34, 21-inch
monitors [1] p. 6, the 640×480 maximum video window at 16-bit color [1] p. 6, 24-bit graphics [1] p. 5, and —
uniquely in the family — "power control service to its expansion slots, so plug-in cards can turn the computer
on and off" [1] pp. 5–6. RAM: four SIMM slots, no memory soldered, 128 MB maximum [1] pp. 3, 13.

### 6.2 Macintosh Centris/Quadra 660AV — [q660av.md](q660av.md)

The shipping 25 MHz Tempest: "a less expensive desktop version of the Macintosh Quadra 840AV in the same
low-profile enclosure as the Macintosh Centris 610" [1] p. 3. Identity: strap nibble $B (§2.4), Gestalt/SysEnvirons
machine ID 60 [1] p. 8, box flag 54 [4]; renamed Quadra 660AV when Apple retired the Centris name. The low-grade
values throughout: BClk 25.0000 MHz, CKI 55.5000 MHz (§5.5), 70 ns DRAM acceptable [1] p. 6, the §2.5 third-row
speed patterns. Expansion is the machine's defining delta:

- **NuBus by adapter only.** One short card, at slot address $C, on "an adapter card that places its NuBus
  accessory card parallel to the main circuit board"; "the adapter card carries the MUNI chip, so this chip is
  present in the system only when the adapter card is installed" [1] p. 40. Hence the family's MUNI-probing
  rule (§5.4) and the 660AV's deliberately feature-less decoder record [3].
- **A processor-direct slot.** "The Macintosh Centris 660AV (but not the Macintosh Quadra 840AV) can accept an
  accessory card that plugs directly into the main circuit board … An accessory card plugged into the main
  circuit board can gain access to the processor as well as to the DAV bus" [1] p. 46. The connector is an AMP
  650231-3, pin-compatible with the Centris 610's 650231-5 [1] p. 46; its 140 pin assignments, the restricted
  signals (the PSC-sourced `/IPL` lines among them, "not to be used as wire-OR lines" [1] Table 3-22 p. 49),
  the 40/20 pF capacitive load limits, and the bus-mastering prohibition — the 660AV "does not support using
  an accessory card as a bus master in addition to the existing bus masters (the processor, the DSP, the PSC,
  and the MUNI)" [1] p. 49 — are the machine page's subject.
- **No slot power control** [1] p. 6; the machine has a hard power switch instead of soft power-off (its
  feature record in the ROM carries the hard-power-off flag where the 840AV's does not [3], *observed*).

Fixed at 1 MB VRAM [1] p. 6, a 512×384 maximum video window at 16 bits [1] p. 6, 16-bit maximum graphics depth
[1] p. 5, no 21-inch monitors [1] p. 6, 4 MB of RAM soldered with two SIMM slots, 68 MB maximum [1] pp. 3, 13,
and room for two internal SCSI devices [1] p. 3.

### 6.3 The unshipped speed grades

The ROM's product table carries two variants no Apple product used: a **33 MHz Cyclone** (strap nibble $7,
Gestalt machine ID 43, box flag 37) and a **33 MHz Tempest** (strap nibble $8, Gestalt machine ID 79, box flag
73) [3] (*observed* in the ROM's machine tables). Their records are complete — RAM, video and slot information,
speed patterns of the middle row of §2.5, MUNI control value $18 — so the boot ROM would bring either board up
without modification; whether any such board was ever built is unknown (§7). The developer note does not mention
them; its identification table lists only the two shipping models [1] p. 8.

### 6.4 What is deliberately identical

Everything not listed above is byte-identical between the machines by construction: one 2 MB ROM image (§2.9),
one decoder record layout, one interrupt architecture (§5.1), one I/O island (§3.2), one PSC, one CIVIC driver
with per-machine parameter records inside it [3] (*observed*: the ROM's video records differ only in the board
sResource ID, 37-class for Cyclone and 54-class for Tempest), and byte-identical per-variant Ethernet
configuration resources [3]. The two machines' shared software — System 7.1 plus one enabler — is gated on the
two box flags and nothing else [4].

---

## 7. Open questions

1. **CPU-bus arbitration policy.** The four masters' priorities, any bus parking, and worst-case grant latency
   are documented nowhere; the developer note names the YMCA as arbiter [1] p. 18 and nothing more. Only the
   two timeout rules (§5.2) are stated.
2. **The YMCA speed bits' actual meaning.** The ROM writes three fixed patterns for three clock grades
   (§2.5); what DRAMspeed/CPUspeed/ROMspeed program inside the controller — CAS latency, wait states, burst
   length — is unknown, as is the encoding of the per-bank size codes below the 16 MB code the sizing path
   writes.
3. **The never-written YMCA registers.** DSPspeed, EPROMmode, 040Mode, Test_Mode and Refresh_Test (§2.3) are
   never touched by any shipped software; their functions are unknown, and so are the reset values of every
   latch in the file. Whether reads of the write-only latches return anything is also untested.
4. **How the startup ROM overlay clears.** The developer note's "after this first access, ROM is no longer
   mapped to the bottom of the RAM space" [1] p. 20 does not say *which* first access; no software action
   clears the overlay (§3.5), so the trigger — first entry-point fetch, first $40800000-window access, first
   RAM write — is undetermined.
5. **DRAM interleaving.** No interleave register exists in the YMCA file and no shipped software writes one
   (§2.6); if paired-bank interleaving happens, it is automatic and invisible. Whether it happens at all is
   unverified.
6. **The I/O island's alias rule and its undecoded offsets.** Which address bits the decoder ignores in
   equating $50036000 with $50F36000 (§3.2, [CIVIC](civic.md) §2.1), and what an access to an undecoded offset
   inside the island returns — data, a floating value, or a bus error — are both unestablished; the only
   deliberate in-island fault on the platform is the absent MUNI of a 660AV.
7. **MUNI's undocumented surface.** The IntStatus, Status and Test registers (§5.4) are never read; the
   meaning of the Control values beyond "clock grade", the conditions that would raise the MUNI interrupt
   (disabled by the boot ROM and never handled), and the block-attempt bits' exact relationship to NuBus '90
   burst negotiation are all open. The BlkAttmpt register's read behavior — the previous mask returning
   shifted — is taken from the driver's handling of it, not from any description of the hardware.
8. **The 0.5 ms interrupt rule's enforcement.** Whether anything enforces "do not disable interrupts for
   longer than 0.5 ms" [1] p. 7 — a watchdog, a PSC timeout — or whether it is purely a software convention,
   is not documented.
9. **The level-6 60 Hz timer's source and rate**, and its relationship to VIA1's retrace tick on CA1
   ([PSC](psc.md) §6 carries the same hole from the PSC side).
10. **Development-stage boards.** The shipped software set contains traces of a pre-release Cyclone: a
    different CPU-ID value associated with an alternative VIA2 register layout, and an alternative pairing of
    the MUNI and Sebastian base addresses ([PSC](psc.md) §2.8 records the VIA2 form; the base swap is
    *inferred — unverified*). No shipping board is affected; the exact population of EVT boards is unknown.
11. **The unshipped 33 MHz grades** (§6.3): whether Cyclone 33 or Tempest 33 hardware ever existed in any
    form, and whether their product records' Gestalt values (43, 79) were ever exposed to software.
12. **The DSP-to-I/O-bus question.** The DSP is a CPU-bus master (§5.2) and its programs live in RAM; whether
    it can also reach the I/O island — the PSC's channels cannot ([1] p. 29) — is not stated anywhere, and no
    shipped DSP code attempts it.
13. **Slot power control on the 840AV.** The feature is documented as a capability [1] pp. 5–6, but which
    slot pin carries it, its current budget, and the protocol a card uses are not in the developer note.
14. **What the E-Disk aperture decodes.** The ROM reserves $70000000–$7FFFFFFF for a RAM disk (§3.1); whether
    the YMCA enforces that reservation in hardware or it is purely an MMU-level convention is not established.
15. **The 24-bit slot-9 repurposing's intent.** That the second ROM megabyte appears at $900000 (§3.4) is
    observed; *why* the platform maps ROM into a slot window — a compatibility measure for 24-bit-only
    software, a boot-time necessity, or an arbitrary choice — is not documented.

---

## References

1. Apple Computer, Inc., *Developer Note: Macintosh Quadra 840AV and Macintosh Centris 660AV Computers*,
   Developer Press, 1993 — §"Models and Accessories" p. 3 (enclosures, speeds, slot counts, RAM maxima);
   §"Summary of Features" pp. 5–6 (PSC DMA, video input/output, DSP, MUNI, New Age, GeoPort, sound, ROM,
   slots, power control); §"Differences Between Models" p. 6; §"Compatibility Issues" p. 7 (the 0.5 ms rule);
   §"Machine Identification" Table 1-1 p. 8 (SysEnvirons 78/60); §"Physical Forms" p. 10; §"Parts Layout" and
   §"System Architecture" p. 12; Chapter 2: §"Functional Units" pp. 12–17 (main processor p. 12, RAM p. 13,
   MCA p. 13, DSP p. 13, PSC p. 13, MUNI p. 14, CIVIC p. 14, Sebastian pp. 14–15, VDC p. 15, Mickey p. 15,
   New Age pp. 15–16, Curio p. 16, ATECS p. 16, Cuda p. 16, Singer p. 16, Endeavor/Clifton Plus p. 17, DMSD
   p. 17), §"System Clocks" Table 2-2 pp. 17–18, §"Signal Buses" p. 18, §"Bus Arbitration" p. 18,
   §"Bus Timeouts" p. 19, §"ROM and RAM Management" p. 19, §"DRAM Configurations" p. 19, §"Startup Memory
   Addressing" p. 20, §"Access Timing" Table 2-3 p. 20, §"External Device Interfaces" pp. 21–23, §"PSC
   Functions" pp. 29–30 (nine channels, the two-register-set latency rationale, arbitration priorities
   Table 2-11), §"Video and Graphics I/O" pp. 30–31, §"External Video Input" p. 32, §"Video RAM Usage"
   pp. 33–34, §"Sound I/O" p. 38, §"NuBus Interface" p. 39 (slot addresses, MUNI timeout, Table 2-16),
   §"Slot Connections" p. 40, §"Digital Audio/Video Expansion Connector" pp. 42–45, §"Processor-Direct Cards
   for the Macintosh Centris 660AV" pp. 46–51 (Table 2-21; Table 3-22 p. 49), §"RAM Expansion Cards" p. 52,
   §"VRAM Expansion Cards" pp. 54–55; Chapter 10 §"Interrupt Handling" p. 407 and §"DMA Use" p. 408; Chapter 11
   §"Video Television Output" p. 410 and §"NuBus Block Moves" pp. 411–412.
2. Apple Computer, Inc., *Guide to the Macintosh Family Hardware*, second edition, Addison-Wesley Publishing
   Company, 1990 — the classic architecture the AV platform inherits: §"The one-second interrupt" p. 145;
   §"Functions of VIA2" pp. 155–157 (VIA2's absorption into custom ICs as the precedent for the PSC's windows);
   Table 4-27 p. 186 (the classic VIA2 interrupt-flag bit assignments the pseudo-VIA2 window descends from).
3. Macintosh Quadra 840AV / Centris 660AV boot ROM (2 MB mask ROM, release $10F3, version word $077D, image
   checksum $5BF10FD1, mapped at $40800000; `CPU address = $40800000 + file offset`) — annotated disassembly,
   resource-container decode and data-table analysis, plus instruction-operand-aware byte scans of the whole
   image. Cited sites: the ROM header and cold-start path (checksum-as-SSP; reset PC $0000002A at offset $04;
   the start-up entry at $40800074 with its CACR/transparent-translation prologue; the MMU-off I/O
   translations covering the $5xxxxxxx region); the hardware-init block at $4080F1D0 (GetHardwareInfo,
   relocation, VIA and Cuda initialization, the YMCA speed dispatch $4080F3B2–$4080F49F with the three
   MUNI_Control writes at $4080F41A/$4080F45C/$4080F496); GetCPUIDReg $4080FDA0 (the $5FFFFFFC read, the
   $A55A signature test, the not-writable test); the strap reads $4080F6EC–$4080F70E and the identity-failure
   loop at $4080F762; the RAM-sizing path at $4080ADAF and the width-register writes; the MMU page templates
   and per-family physical/decoder tables (the I/O island and its $50F40000 alias, the $70000000 E-Disk
   aperture, the 24-bit template with the $900000 ROM alias, the per-variant product records with their
   Gestalt and box-flag values, the deliberately feature-less 660AV expansion record); the MUNI probe under
   the bus-error handler and the interrupt-disable write to $50F30000; the level-3–6 dispatchers
   $40810830–$408109C0; the four byte-identical Ethernet configuration resources at file offsets
   $1AEE30/$1AEE90/$1AEEF0/$1AEF50; the universal declaration block with its "Friday, June 25, 1993" date at
   $1FFF4A; the boot chime PCM at file offset $C5D24; the byte scans establishing that $50F36000 is never
   referenced and that no other $50F30400-class YMCA offset is touched.
4. Apple Computer, Inc., System Enabler 088 (System 7.1 enabler for the Macintosh Quadra 840AV and Macintosh
   Centris 660AV, 1993) — resource inventory and decode: the machine-gating header resource (box flags $48 = 72
   and $36 = 54, and no others) and the enabler's component set — the serial HAL, the video digitizer, the
   sound component stack and the DSP sample-rate-converter module — whose ROM-resident predecessors the
   enabler's builds supersede.
5. Apple Computer, Inc., *Designing Cards and Drivers for the Macintosh Family*, third edition, Addison-Wesley
   Publishing Company, 1992 — NuBus as the AV platform implements it: standard (minor) and super (major) slot
   space, the slot-ID/decode relationship, declaration-ROM format and the Slot Manager's search, and the
   block-transfer sResources the MUNI's slave-size-4 rule consumes.
