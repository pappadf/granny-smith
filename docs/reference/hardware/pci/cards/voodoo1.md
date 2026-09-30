# 3dfx Voodoo Graphics (SST-1)

The **3dfx Voodoo Graphics** — 3dfx's internal name **SST-1** — is the first-generation 3D-only
graphics accelerator chipset: a pair of ASICs, one frame-buffer interface (FBI) and one to three
texture-mapping units (TREX), that renders full-screen 3D into its own private EDO DRAM and
passes the host's 2D video signal through to the monitor while no 3D context is active. It is
not a display card in the ordinary sense: it has no 2D engine the host addresses as a
framebuffer, it carries no expansion ROM of any kind, it cannot be the boot display of any
machine it plugs into, and it cannot even interrupt the host — every command, texture and pixel
is pushed to it over PCI and its completion is polled. On the PCI Power Macintosh it is a
consumer add-on, sold as the **TechWorks Power3D**, that turns QuickDraw 3D RAVE and Glide
titles into accelerated full-screen 3D while the machine's built-in video keeps the desktop.

This page documents the hardware: the 16 MB memory-mapped aperture and its complete register
file, the pixel pipeline and texture engine, the video backend and its external DAC, and the
software contract — the 3dfx register specification [1] and Glide manuals [2] [3] as the
primary authority, the open-sourced Glide bring-up code [5] and a second independent operating
system driver [6] for the programming sequences, and the shipped Macintosh driver of
1997-08-27, whose archive opens directly and whose file names and type/creator codes are read
here [7]. The host side of the bus — configuration cycles, BAR assignment, the memory windows —
is the PCI page's subject ([pci.md](../pci.md)); the machine that implements it on the
computers these cards shipped into is [bandit.md](../../../machines/tnt/bandit.md), the family
map [tnt.md](../../../machines/tnt/tnt.md) §3. The second generation — Voodoo2, which inherits
this register architecture and extends it with on-chip triangle setup, a second texture unit, a
2D blitter, a command FIFO and real interrupts — has its own page
([voodoo2.md](voodoo2.md)); the generation split is kept clean there, and this page documents
only SST-1 silicon and the first-generation software.

**Contents:**

1. [Overview](#1-overview) — what the part is, FBI and TREX, the Mac cards, the ROM-less
   identity, clocking, memory configurations and tiling
2. [Register file](#2-register-file) — the 16 MB aperture; the wrap/chip/register/byte address
   decode; the complete register map in both mappings; status; the triangle parameters and
   command registers; the rendering state; lfbMode; constants, counters and the fog table;
   video timing, CLUT and DAC; fbiInit0–fbiInit4; the TREX register file; PCI configuration
   space
3. [Behaviour](#3-behaviour) — the PCI slave and its FIFOs; the pixel pipeline; triangle
   rendering and sub-pixel correction; linear framebuffer access; texture memory; video
   output; the interrupt that does not exist; passthrough and bus snooping; buffer swaps;
   scanline interleaving
4. [Programming model](#4-programming-model) — power-on bring-up, DAC detection and clocks, a
   real mode-set, the Glide contract, the 1997 Power3D driver, Mac-side discovery, endianness,
   the configuration variables
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 What the part is

"SST-1 (a.k.a. Voodoo Graphics™) — High Performance Graphics Engine for 3D Game
Acceleration" is 3dfx's own specification title; the document studied here is revision 1.61,
dated December 1, 1999, carrying a 1995–1999 copyright and a revision history that runs from
0.7 to 1.61 [1] title page, §13 p. 90. The part is "the first video subsystem that enables
personal computers and low cost video game platforms to host true 3D entertainment
applications", optimized for real-time texture-mapped 3D games: "true-perspective texture
mapping with trilinear mipmapping and lighting, detail and projected texture mapping, texture
anti-aliasing, and high precision sub-pixel correction", plus "polygonal-based Gouraud shading,
depth-buffering, alpha blending, and dithering" [1] §1 p. 6. Glide's own manual opens with the
same sentence and names the market-facing chips: the subsystem "consists of two 3Dfx
Interactive proprietary ASICs, **Texelfx** and **Pixelfx**, and memory" — Texelfx carrying the
TMU, Pixelfx the frame-buffer interface [2] Ch. 1 p. 2. The specification's names for the same
parts are TREX and FBI; both name sets appear throughout the evidence and mean the same silicon
[1] §3.1 p. 8, [2] Ch. 1 p. 2.

The defining properties, all from the specification's own summary and functional overview:

- It is **3D-only**: no 2D core, no VGA controller, no BIOS. The host's existing 2D card feeds
  its monitor output through the Voodoo Graphics board, and the Voodoo's own video backend
  takes over only while a 3D context is active (§3.8). "SST-1 supports 16-bit RGB buffer
  displays only" — the host may hand it 24-bit pixels, which are dithered down at the back end
  of the pixel pipeline [1] §3.3 p. 16.
- It is a **PCI slave only**: "SST-1 implements the PCI bus protocol, and conforms to PCI bus
  specification 2.1. SST-1 is a slave only device, and supports zero-wait-state and burst
  transfers" [1] §3.3 p. 13. There is no DMA in either direction; the host CPU pushes
  everything. The configuration space accordingly hardwires the bus-mastering registers to
  zero (§2.15).
- It occupies **16 MB of memory-mapped PCI address space and no I/O space**: "SST-1 does not
  utilize I/O mapped address space", and "SST-1 does not support I/O accesses. All I/O accesses
  to SST-1 are ignored" [1] §4 p. 17, §9.1 p. 84.
- It is **bi-endian in the datapaths that matter to a big-endian host**: the linear framebuffer
  carries explicit byte-swizzle and word-swap controls with the design intent stated outright —
  "for little endian CPUs (e.g. Intel x86 processors) byte swizzling should not be enabled,
  however **big endian CPUs (e.g. PowerPC processors) should enable byte swizzling**" [1]
  §5.20 pp. 48–49. This is the single most Mac-relevant silicon feature (§4.7).
- It **cannot interrupt**: the interrupt pin is specified and the interrupt-line register
  defaults to a PC IRQ number, but the interrupt status bit "IS CURRENTLY NOT IMPLEMENTED IN
  HARDWARE, AND WILL ALWAYS RETURN 0x0" [1] §5.1 p. 24 (§3.7).

### 1.2 FBI and TREX

"In its entry level configuration, a SST-1 graphics solution consists of two rendering ASICs:
TREX and FBI" [1] §3.1 p. 8:

- **FBI** ("Frame Buffer Interface") is the PCI slave — "all communication from the host CPU to
  the SST-1 graphics subsystem is performed through FBI". It implements Gouraud shading, alpha
  blending, depth-buffering and dithering, the programmable fog table, all linear framebuffer
  access, and the video display controller that drives the monitor [1] §3.1 p. 8.
- **TREX** ("Texture Raster Engine") "implements all texture mapping capabilities": the
  per-pixel perspective divide (true-perspective mapping divides by W every pixel), LOD
  mipmapping, bilinear filtering, and the advanced modes — detail texture mapping, projected
  texture mapping, trilinear filtering [1] §3.1 p. 8. TREX has its own dedicated 64-bit
  datapath to its own texture memory, fully interleaved so that "bilinear texture filtering
  with no performance penalty relative to point sampling" costs nothing, and "texels are not
  duplicated in texture memory" [1] §3.3 p. 13.

The TREX count is the configurability axis. With a single TREX, the advanced texture mapping
techniques (detail, projected, trilinear) are two-pass operations; with two TREX ASICs each
becomes single-pass; with three, all supported features including projected-plus-detail
trilinear become single-pass [1] §3.1 pp. 8–10 (the spec's own one/two/three-TREX pass table).
TREX has "a dedicated expansion bus which allows multiple TREX ASICs to be chained together",
and beyond that, "multiple FBI/TREX subsystems can be chained together utilizing scan-line
interleaving to effectively double the rendering rate" [1] §3.1 pp. 8–10 (§3.10). The retail
card the Mac market saw is the single-TREX, 4 MB configuration: "A typical Voodoo Graphics PCI
expansion card consisted of a DAC, a frame buffer processor and a texture mapping unit, along
with 4 MB of EDO DRAM. The RAM and graphics processors operated at 50 MHz" [11].

The feature list continues: linearly interpolated 16-bit Z-buffering *or* the patent-pending
perspective-corrected 16-bit floating-point W-buffer (using the already-iterated 1/W as the
depth component, so "the host CPU no longer needs to setup the Z component for a given
polygon"), source/destination alpha blending, chroma-key transparency with a dedicated color
mask, sub-pixel correction to 0.4 × 0.4, 4×4 or 2×2 ordered dithering, a gamma-correction
color lookup table, ten texture formats from 8-bit RGB(3-3-2) to 16-bit alpha-intensity(8-8),
and the "narrow channel" YIQ texture compression performed by the host CPU [1] §1 pp. 6–7,
§3.3 pp. 13–16.

### 1.3 The Mac cards

3dfx never built a Mac-specific Voodoo Graphics board; the products sold into the PCI Power
Macintosh market are rebranded reference designs plus one Mac-only third-party variant:

| Card | Silicon | Memory | Evidence |
|---|---|---|---|
| TechWorks Power3D | SST-1, FBI + one TREX, 50 MHz | 4 MB EDO (2 MB framebuffer + 2 MB texture) | [9] [10] [11] |
| Village Tronic MacMagic | Voodoo Graphics | 8 MB | [12] (drivers only; board details unverified, §6.2) |

The **TechWorks Power3D** is the PC reference design with Mac software and a Mac video
pass-through: the Mac's built-in video connector is DB-15, so the Mac card carries a DB-15
loop where PC cards carry HD-15 — the same cabling difference as the second-generation cards
([voodoo2.md](voodoo2.md) §1.3; *inferred* from the host connector, as no Power3D schematic
exists in the evidence set). The card was sold in 1997 with TechWorks's own driver CD
(publication date 1997 [10]) and a bundled game; TechWorks's own marketing copy for the
package reads: "Turns your PowerPC into a Power Arcade! Provides full-screen 3D acceleration.
Supports up to 32,000 (16-bit) colors. Supports 640x480 full-screen resolution. Easy install —
works with existing 2D video. Works on any PowerPCs with a PCI slot. Supports QuickDraw 3D
RAVE games. Exclusive — native 3Dfx Glide API support!", with system requirements "MacOS 7.5.3
or greater, PowerPC with at least one available PCI expansion slot (except 'all-in-one'
systems), 32MB RAM" [9]. The performance claims in the same copy — "45 megapixels/sec.
sustained polygon fill with all 3D features enabled", "over 1 million triangles per second
throughput on filtered, mip-mapped, Z-buffered, alpha-blended, fogged, textured 25-pixel
triangles" [9] — track the specification's own 50 MHz tables (968 Ktri/s for the fully
featured 25-pixel case, 100 Mpixels/s peak clear rate, §1.4) [1] §2 p. 7.

The **Village Tronic MacMagic** is the exception in the family: an 8 MB Voodoo Graphics board
from a Mac-only vendor, with drivers on Village Tronic's own driver CD alongside the
MacPicasso line [12]. Its frequently repeated distinguishing claim — a Mac-specific ROM that
makes it PC-incompatible — is single-sourced in the evidence set and unverified here (§6.2);
the drivers held are installers, and the board itself is not in the evidence set.

The pass-through switch itself is a card-design choice, not a chipset one: "The method used to
engage the Voodoo's output circuitry varied between cards, with some using mechanical relays
while others utilized purely solid-state components. The mechanical relays emitted an audible
'clicking' sound when they engaged and disengaged" [11]. Which method the Power3D uses is not
documented (§6.3).

### 1.4 The ROM-less identity and the PCI ID

The Voodoo Graphics board carries **no expansion ROM**: no VGA BIOS, no Open Firmware FCode, no
name-producing device firmware. A Macintosh booting Open Firmware therefore does what Apple's
PCI book specifies for "an expansion ROM with no FCode, or no expansion ROM at all": "the card
is recognized and address space is allocated for the device, but no peripheral initialization
or driver code is loaded... there is no distinct name property for the device" [4]
§"No Open Firmware Support" p. 88. Concretely, on a PCI Power Mac the card is enumerated on
the bridge's bus, its single 16 MB memory BAR is probed (all-ones write, size read-back — the
sequence in [pci.md](../pci.md) §4.3) and assigned a PCI memory address inside a bridge window
([bandit.md](../../../machines/tnt/bandit.md) §4.2; the window it lands in is
[tnt.md](../../../machines/tnt/tnt.md) §3.3), and the Open Firmware device tree records the
node with `reg`/`assigned-addresses` properties but no display-driver FCode — the card is
invisible as a boot device, the built-in video remains the startup screen, and the node carries
only the standard PCI identity naming ([pci.md](../pci.md) §2.7–2.8). The card cannot be the
primary display under any circumstances: it has no 2D mode, and its video backend only outputs
while a Glide/RAVE context holds the passthrough inactive (§3.8).

All device identity therefore flows through PCI configuration space: **vendor ID 0x121A**
(3Dfx Interactive — "Default is 0x121a" [1] §6.1 p. 72), **device ID 0x0001** ("SST-1 Device
Identification. Default is 0x1" [1] §6.2 p. 72) — the value the shipping hardware reports, per
the two independent drivers that match on it: the Glide SST-1 init scans with
`deviceID = 1; /* Find only sst1 boards */` [5] `sst1init.c`, and the Linux PCI ID table
defines `0x121a` / `0x0001` as Voodoo Graphics [6]. The class code is the silent oddity: the
SST-1 hardwires it to zero [1] §6.6 p. 73 — in configuration space the device is not a display
controller at all, unlike the strap-selected display class codes of the second generation
([voodoo2.md](voodoo2.md) §2.12).

Because the second generation's specification was written as a delta on this one, the
generation boundary in the documentation is worth stating once: everything in the SST-1
register file's rendering path — vertex parameters, the TRIANGLE command, fbzColorPath,
fogMode, alphaMode, fbzMode, lfbMode, the clip and constant registers, FASTFILL, SWAPBUFFER,
the fbiInit family — reappears in Voodoo2 with wider fields and extra registers, and the
Voodoo2 specification defers to the SST-1 document where the two coincide ([voodoo2.md](voodoo2.md)
§1.1). What Voodoo2 *adds* — on-chip triangle setup, the second and third TMU, the 2D blitter,
CMDFIFO, real interrupts, the fbiInit5–7 extension — is that page's subject.

### 1.5 Clocking, memory configurations and tiling

The specification's memory-system facts are stated at a 50 MHz graphics clock driving 50-ns
EDO DRAM, and its performance tables are computed at 640×480@60 Hz on that assumption [1]
§2 p. 7. FBI contains "a 64-bit wide interleaved datapath to RGB and alpha/depth-buffer
memory"; for Gouraud or textured polygons with depth buffering enabled one pixel is written
per clock — a 50 Mpixel/s peak fill rate — and screen/depth clears write two pixels per clock,
100 Mpixel/s [1] §3.3 p. 13. Frame buffer memory is 2 MB minimum, 4 MB maximum; texture memory
1 MB minimum, 8 MB maximum [1] §3.3 p. 13 (the feature list prints "1–4 MBytes" for texture
memory [1] §1 p. 6 — the larger §3.3 figure is the later, revised statement; the retail Mac
card's 2 MB is far below either bound). Both controllers support standard, EDO and
synchronous DRAM "to provide a wide range of price/performance options" [1] §3.1 p. 8.

The framebuffer budget, from the specification's own resolution table [1] §1 p. 6 and
Glide's copy of it [2] Ch. 3 p. 23:

| Frame buffer | Double buffered, no depth | Triple buffered, no depth | Double buffered + 16-bit depth |
|---|---|---|---|
| 2 MB | 800×600×16 | 640×480×16 | 640×480×16 |
| 4 MB | 800×600×16 | 800×600×16 | 800×600×16 |

Framebuffer memory is tiled in **64×16-pixel tiles** — the count of tiles in X being a
`fbiInit1` field (§2.13) — a layout the second generation explicitly changed ("Changed tiling
algorithm from 64x16 tiles to 32x32 tiles", recorded in the Voodoo2 revision history,
[voodoo2.md](voodoo2.md) §1.5). The video timing controller is fully programmable, "which
allows for very flexible video timing. Any monitor type may be used with SST-1, with 76+ Hz
vertical refresh rates supported at 800x600 resolution, and 100+ Hz vertical refresh rates
supported at 640x480 resolution" [1] §3.3 p. 16. The graphics clock is not fixed by any strap:
it is programmed at driver bring-up through the external DAC's PLL (§4.2), which is why the
same chipset shipped at a spread of clocks and why every driver carries an override knob for
it (§4.8) — the Linux driver's defaults are 50 MHz for Voodoo Graphics, capped at 60 [6]
`sstfb.c`.

## 2. Register file

### 2.1 The 16 MB aperture

Everything the host can touch lives in one 16 MB memory-mapped window, assigned by the
memBaseAddr configuration register: "For memory mapped accesses on the 32-bit PCI bus, the
contents of memBaseAddr are compared with the pci_ad bits 31..24 (upper 8 bits) to determine
if SST-1 is being accessed" [1] §6.11 p. 74 — a 16 MB-granular decode, with the register
defaulting to 0xff000000 and resettable for size probing by "writing 0xffffff to this register"
[1] §6.11 p. 74. A `fbiInit3` read-only strap can hardwire the base instead: bit 12 "Hardcode
PCI base address 0x1f000000 (1=enable)" [1] §5.45 p. 61. The window divides three ways [1]
§4 p. 17:

| Aperture offset | Size | Contents |
|---|---|---|
| 0x000000–0x3FFFFF | 4 MB | SST-1 memory-mapped register set |
| 0x400000–0x7FFFFF | 4 MB | linear frame buffer access port |
| 0x800000–0xFFFFFF | 8 MB | texture memory access port (write only) |

The base is "setup by the PCI System BIOS during system poweron and initialization and should
not be modified by software" [1] §4 p. 17 — on a Power Macintosh, by Open Firmware's BAR
assignment ([pci.md](../pci.md) §4.3).

### 2.2 The register address decode: wrap, chip, register, byte

A register address is the low 22 bits of the PCI address, divided into four fields [1] §5
pp. 18–19:

| Field | Bits (of the 22) | Width | Meaning |
|---|---|---|---|
| Wrap | 21:14 | 8 | aliases 256 copies of the 14-bit register map |
| Chip | 13:10 | 4 | select(s) of FBI and TREX #0–#2 |
| Register | 9:2 | 8 | the register index (× 4 gives the byte offset) |
| Byte | 1:0 | 2 | must be 0 |

The **wrap** field exists for hosts whose write buffers collapse repeated writes to one
address: "The wrap field is useful for processors such as the Digital Alpha AXP which contains
large writebuffers which collapse multiple writes to the same address into a single write (an
undesirable effect when programming SST-1). By writing to different wraps, software can
guarantee that writes are not collapsed in the write buffer. Note that SST-1 functionality is
identical regardless of which wrap is accessed" [1] §5 p. 18.

The **chip** field is a write-only bitmap: "Each bit in the chip field selects one chip for
writing, with FBI controlled by the lsb of the chip field, and TREX #2 controlled by the msb of
the chip field", and value 0 selects all chips [1] §5 p. 18. The full mapping [1] §5 p. 18:

| Chip field | Chips written |
|---|---|
| 0000 / 1111 | FBI + all TREX chips |
| xxx1 | FBI |
| xx1x | TREX #0 |
| x1xx | TREX #1 |
| 1xxx | TREX #2 |

**Reads always come from FBI**: "for reads, the chip field is ignored, and read data is always
read from FBI" [1] §5 p. 18 — reading a TREX-resident register returns undefined data [1] §5
p. 19.

All register accesses are 32-bit: "All accesses to the memory mapped registers must be
32-bit accesses. No byte (8-bit) or halfword (16-bit) accesses are allowed... so the byte
(2-bit) field of all memory mapped register accesses must be 0x0. As a result, to modify
individual bits of a 32-bit register, the entire 32-bit word must be written with valid bits in
all positions" [1] §5 p. 19.

### 2.3 Register map (normal mapping)

The specification's own register table [1] §5 pp. 18–23. The chip column: "FBI+TREX\*"
registers are written to FBI and to each TREX selected by the chip field; a "\*" alone marks a
register "only written to a given TREX if specified in the chip address", "\%" would mean
unconditional (none in this table). The two trailing columns are the specification's
*Sync?* / *FIFO?* pair: "yes" in Sync means "the graphics processor will flush the data
pipeline before loading the register — this will result in a small performance degradation";
"yes" in FIFO means the write is pushed into the PCI bus FIFO. "Reads are not pushed into the
PCI bus FIFO, and reading FIFOed registers will return the current value of the register,
irrespective of pending writes to the register present in the FIFO" [1] §5 p. 19. Reading a
write-only register returns undefined data; reserved bits of readable registers are undefined
and must be masked [1] §5 p. 19.

| Register | Address | Bits | Chip | R/W | Sync/FIFO | Function |
|---|---|---|---|---|---|---|
| status | 0x000 | 31:0 | FBI | R/W | No/Yes | status (§2.5) |
| *reserved* | 0x004 | | | | | |
| vertexAx | 0x008 | 15:0 | FBI+TREX\* | W | No/Yes | vertex A x (12.4) |
| vertexAy | 0x00C | 15:0 | FBI+TREX\* | W | No/Yes | vertex A y (12.4) |
| vertexBx | 0x010 | 15:0 | FBI+TREX\* | W | No/Yes | vertex B x (12.4) |
| vertexBy | 0x014 | 15:0 | FBI+TREX\* | W | No/Yes | vertex B y (12.4) |
| vertexCx | 0x018 | 15:0 | FBI+TREX\* | W | No/Yes | vertex C x (12.4) |
| vertexCy | 0x01C | 15:0 | FBI+TREX\* | W | No/Yes | vertex C y (12.4) |
| startR | 0x020 | 23:0 | FBI | W | No/Yes | starting red (12.12) |
| startG | 0x024 | 23:0 | FBI | W | No/Yes | starting green (12.12) |
| startB | 0x028 | 23:0 | FBI | W | No/Yes | starting blue (12.12) |
| startZ | 0x02C | 31:0 | FBI | W | No/Yes | starting Z (20.12) |
| startA | 0x030 | 23:0 | FBI | W | No/Yes | starting alpha (12.12) |
| startS | 0x034 | 31:0 | TREX\* | W | No/Yes | starting S/W (14.18) |
| startT | 0x038 | 31:0 | TREX\* | W | No/Yes | starting T/W (14.18) |
| startW | 0x03C | 31:0 | FBI+TREX\* | W | No/Yes | starting 1/W (2.30) |
| dRdX | 0x040 | 23:0 | FBI | W | No/Yes | dRed/dX (12.12) |
| dGdX | 0x044 | 23:0 | FBI | W | No/Yes | dGreen/dX (12.12) |
| dBdX | 0x048 | 23:0 | FBI | W | No/Yes | dBlue/dX (12.12) |
| dZdX | 0x04C | 31:0 | FBI | W | No/Yes | dZ/dX (20.12) |
| dAdX | 0x050 | 23:0 | FBI | W | No/Yes | dAlpha/dX (12.12) |
| dSdX | 0x054 | 31:0 | TREX\* | W | No/Yes | d(S/W)/dX (14.18) |
| dTdX | 0x058 | 31:0 | TREX\* | W | No/Yes | d(T/W)/dX (14.18) |
| dWdX | 0x05C | 31:0 | FBI+TREX\* | W | No/Yes | d(1/W)/dX (2.30) |
| dRdY | 0x060 | 23:0 | FBI | W | No/Yes | dRed/dY (12.12) |
| dGdY | 0x064 | 23:0 | FBI | W | No/Yes | dGreen/dY (12.12) |
| dBdY | 0x068 | 23:0 | FBI | W | No/Yes | dBlue/dY (12.12) |
| dZdY | 0x06C | 31:0 | FBI | W | No/Yes | dZ/dY (20.12) |
| dAdY | 0x070 | 23:0 | FBI | W | No/Yes | dAlpha/dY (12.12) |
| dSdY | 0x074 | 31:0 | TREX\* | W | No/Yes | d(S/W)/dY (14.18) |
| dTdY | 0x078 | 31:0 | TREX\* | W | No/Yes | d(T/W)/dY (14.18) |
| dWdY | 0x07C | 31:0 | FBI+TREX\* | W | No/Yes | d(1/W)/dY (2.30) |
| triangleCMD | 0x080 | 31 | FBI+TREX\* | W | No/Yes | execute TRIANGLE (fixed point) |
| *reserved* | 0x084 | | | | | |
| fvertexAx | 0x088 | 31:0 | FBI+TREX\* | W | No/Yes | vertex A x (IEEE float) |
| fvertexAy | 0x08C | 31:0 | FBI+TREX\* | W | No/Yes | vertex A y (float) |
| fvertexBx | 0x090 | 31:0 | FBI+TREX\* | W | No/Yes | vertex B x (float) |
| fvertexBy | 0x094 | 31:0 | FBI+TREX\* | W | No/Yes | vertex B y (float) |
| fvertexCx | 0x098 | 31:0 | FBI+TREX\* | W | No/Yes | vertex C x (float) |
| fvertexCy | 0x09C | 31:0 | FBI+TREX\* | W | No/Yes | vertex C y (float) |
| fstartR | 0x0A0 | 31:0 | FBI | W | No/Yes | starting red (float) |
| fstartG | 0x0A4 | 31:0 | FBI | W | No/Yes | starting green (float) |
| fstartB | 0x0A8 | 31:0 | FBI | W | No/Yes | starting blue (float) |
| fstartZ | 0x0AC | 31:0 | FBI | W | No/Yes | starting Z (float) |
| fstartA | 0x0B0 | 31:0 | FBI | W | No/Yes | starting alpha (float) |
| fstartS | 0x0B4 | 31:0 | TREX\* | W | No/Yes | starting S/W (float) |
| fstartT | 0x0B8 | 31:0 | TREX\* | W | No/Yes | starting T/W (float) |
| fstartW | 0x0BC | 31:0 | FBI+TREX\* | W | No/Yes | starting 1/W (float) |
| fdRdX | 0x0C0 | 31:0 | FBI | W | No/Yes | dRed/dX (float) |
| fdGdX | 0x0C4 | 31:0 | FBI | W | No/Yes | dGreen/dX (float) |
| fdBdX | 0x0C8 | 31:0 | FBI | W | No/Yes | dBlue/dX (float) |
| fdZdX | 0x0CC | 31:0 | FBI | W | No/Yes | dZ/dX (float) |
| fdAdX | 0x0D0 | 31:0 | FBI | W | No/Yes | dAlpha/dX (float) |
| fdSdX | 0x0D4 | 31:0 | TREX\* | W | No/Yes | d(S/W)/dX (float) |
| fdTdX | 0x0D8 | 31:0 | TREX\* | W | No/Yes | d(T/W)/dX (float) |
| fdWdX | 0x0DC | 31:0 | FBI+TREX\* | W | No/Yes | d(1/W)/dX (float) |
| fdRdY | 0x0E0 | 31:0 | FBI | W | No/Yes | dRed/dY (float) |
| fdGdY | 0x0E4 | 31:0 | FBI | W | No/Yes | dGreen/dY (float) |
| fdBdY | 0x0E8 | 31:0 | FBI | W | No/Yes | dBlue/dY (float) |
| fdZdY | 0x0EC | 31:0 | FBI | W | No/Yes | dZ/dY (float) |
| fdAdY | 0x0F0 | 31:0 | FBI | W | No/Yes | dAlpha/dY (float) |
| fdSdY | 0x0F4 | 31:0 | TREX\* | W | No/Yes | d(S/W)/dY (float) |
| fdTdY | 0x0F8 | 31:0 | TREX\* | W | No/Yes | d(T/W)/dY (float) |
| fdWdY | 0x0FC | 31:0 | FBI+TREX\* | W | No/Yes | d(1/W)/dY (float) |
| ftriangleCMD | 0x100 | 31 | FBI+TREX\* | W | No/Yes | execute TRIANGLE (float) |
| fbzColorPath | 0x104 | 27:0 | FBI+TREX\* | R/W | No/Yes | color/alpha pipeline control (§2.8) |
| fogMode | 0x108 | 5:0 | FBI | R/W | No/Yes | fog control (§2.8) |
| alphaMode | 0x10C | 31:0 | FBI | R/W | No/Yes | alpha function/blend control (§2.8) |
| fbzMode | 0x110 | 20:0 | FBI | R/W | Yes/Yes | RGB/depth buffer control (§2.9) |
| lfbMode | 0x114 | 16:0 | FBI | R/W | Yes/Yes | linear framebuffer control (§2.10) |
| clipLeftRight | 0x118 | 31:0 | FBI | R/W | Yes/Yes | clip rectangle X (§2.9) |
| clipLowYHighY | 0x11C | 31:0 | FBI | R/W | Yes/Yes | clip rectangle Y (§2.9) |
| nopCMD | 0x120 | 0 | FBI+TREX\* | W | Yes/Yes | execute NOP (§2.7) |
| fastfillCMD | 0x124 | n/a | FBI | W | Yes/Yes | execute FASTFILL (§2.7) |
| swapbufferCMD | 0x128 | 8:0 | FBI | W | Yes/Yes | execute SWAPBUFFER (§2.7) |
| fogColor | 0x12C | 23:0 | FBI | W | Yes/Yes | fog color (§2.11) |
| zaColor | 0x130 | 31:0 | FBI | W | Yes/Yes | constant depth/alpha (§2.11) |
| chromaKey | 0x134 | 23:0 | FBI | W | Yes/Yes | chroma-key color (§2.11) |
| *reserved* | 0x138–0x13C | | | | | |
| stipple | 0x140 | 31:0 | FBI | R/W | Yes/Yes | stipple mask (§2.9) |
| color0 | 0x144 | 31:0 | FBI | R/W | Yes/Yes | constant color 0 (§2.11) |
| color1 | 0x148 | 31:0 | FBI | R/W | Yes/Yes | constant color 1 (§2.11) |
| fbiPixelsIn | 0x14C | 23:0 | FBI | R | | pixels processed (§2.11) |
| fbiChromaFail | 0x150 | 23:0 | FBI | R | | pixels failed chroma test |
| fbiZfuncFail | 0x154 | 23:0 | FBI | R | | pixels failed Z test |
| fbiAfuncFail | 0x158 | 23:0 | FBI | R | | pixels failed alpha test |
| fbiPixelsOut | 0x15C | 23:0 | FBI | R | | pixels drawn |
| fogTable | 0x160–0x1DC | 31:0 | FBI | W | Yes/Yes | fog table, 32 words = 64 entries (§2.11) |
| *reserved* | 0x1E0–0x1FC | | | | | |
| fbiInit4 | 0x200 | 12:0 | FBI | R/W | n/a/No | FBI init 4 (§2.13) |
| vRetrace | 0x204 | 11:0 | FBI | R | n/a/No | vertical retrace counter (§2.12) |
| backPorch | 0x208 | 23:0 | FBI | R/W | n/a/No | video back porch (§2.12) |
| videoDimensions | 0x20C | 25:0 | FBI | R/W | n/a/No | screen dimensions (§2.12) |
| fbiInit0 | 0x210 | 31:0 | FBI | R/W | n/a/No | FBI init 0 (§2.13) |
| fbiInit1 | 0x214 | 31:0 | FBI | R/W | n/a/No | FBI init 1 (§2.13) |
| fbiInit2 | 0x218 | 31:0 | FBI | R/W | n/a/No | FBI init 2 (§2.13) |
| fbiInit3 | 0x21C | 31:0 | FBI | R/W | n/a/No | FBI init 3 (§2.13) |
| hSync | 0x220 | 25:0 | FBI | W | n/a/No | horizontal sync timing (§2.12) |
| vSync | 0x224 | 27:0 | FBI | W | n/a/No | vertical sync timing (§2.12) |
| clutData | 0x228 | 29:0 | FBI | W | n/a/No | internal CLUT write (§2.12) |
| dacData | 0x22C | 31:0 | FBI | W | n/a/No | external DAC access (§2.12) |
| maxRgbDelta | 0x230 | 23:0 | FBI | W | n/a/No | video filter threshold (§2.12) |
| *reserved* | 0x234–0x2FC | | | | | |
| textureMode | 0x300 | 30:0 | TREX | W | No/Yes | texture mode control (§2.14) |
| tLOD | 0x304 | 23:0 | TREX | W | No/Yes | LOD settings (§2.14) |
| tDetail | 0x308 | 16:0 | TREX | W | No/Yes | detail texture control (§2.14) |
| texBaseAddr | 0x30C | 18:0 | TREX | W | No/Yes | texture base, LOD 0 (§2.14) |
| texBaseAddr_1 | 0x310 | 18:0 | TREX | W | No/Yes | texture base, LOD 1 |
| texBaseAddr_2 | 0x314 | 18:0 | TREX | W | No/Yes | texture base, LOD 2 |
| texBaseAddr_3_8 | 0x318 | 18:0 | TREX | W | No/Yes | texture base, LODs 3–8 |
| trexInit0 | 0x31C | 31:0 | TREX | W | Yes/Yes | TREX init 0 (§2.14) |
| trexInit1 | 0x320 | 31:0 | TREX | W | Yes/Yes | TREX init 1 (§2.14) |
| nccTable0 | 0x324–0x350 | 31:0/26:0 | TREX | W | Yes/Yes | NCC table 0 / palette (§2.14) |
| nccTable1 | 0x354–0x380 | 31:0/26:0 | TREX | W | Yes/Yes | NCC table 1 (§2.14) |
| *reserved* | 0x384–0x3FC | | | | | |

### 2.4 Triangle parameter address remapping

"When fbiinit3(0)=1, the triangle parameter registers can be aliased to a different address
mapping to improve PCI bus throughput" — the remap is armed by `fbiInit3` bit 0 and takes
effect only for accesses with the wrap field's upper bit set (pci_ad[21]=1), leaving every
other register untouched [1] §5 pp. 22–23. In the remapped view the vertex registers keep
their normal addresses, but the parameter block is interleaved in **RGBZASTW order** — each
parameter's start and both slopes sit contiguously (startR, dRdX, dRdY, then G, B, Z, A,
S, T, W), from 0x020 through 0x080, followed by the floating-point block in the same order from
0x088 through 0x100 [1] §5 pp. 22–23. The revision history records the ordering change at
document revision 1.40: "Changed triangle parameter address remapping to RGBZASTW order" [1]
§13 p. 90. The point of the layout is burst-friendliness: a whole triangle's state becomes a
contiguous ascending address run.

### 2.5 status and cfgStatus

The status register "provides a way for the CPU to interrogate the graphics processor about its
current state and FIFO availability. The status register is read only, but writing to status
clears any SST-1 generated PCI interrupts" [1] §5.1 p. 24:

| Bits | Meaning | Reset |
|---|---|---|
| 5:0 | PCI FIFO freespace (0x3F = FIFO empty; 64-entry FIFO) | 0x3F |
| 6 | vertical retrace (0 = retrace active) | 1 |
| 7 | FBI graphics engine busy | 0 |
| 8 | TREX busy — set if *any* TREX unit (engine or FIFOs) is not idle | 0 |
| 9 | SST-1 busy — set when any internal unit is active or any FIFO non-empty | 0 |
| 11:10 | displayed buffer (0 = buffer 0, 1 = buffer 1, 2 = auxiliary) | 0 |
| 27:12 | memory FIFO freespace (0xFFFF = empty) | 0xFFFF |
| 30:28 | swap buffers pending (§3.9) | 0 |
| 31 | PCI interrupt generated — **not implemented, always reads 0** | 0 |

Bit 7 "only determines if the graphics engine of FBI is busy — it does not include information
as to the status of the internal PCI FIFOs", so an idle engine can still have queued work;
bit 9 is the one that covers everything [1] §5.1 p. 24. The configuration-space **cfgStatus**
register (offset 0x4C) is a pure alias: "Reading the configuration-space cfgStatus register
will return the same data as if reading from the memory-mapped status register" [1] §6.18
p. 76 — a way to poll without touching the memory aperture.

### 2.6 The triangle parameter registers

A triangle is three vertices in 12.4 fixed point (vertexAx..vertexCy) or IEEE single precision
(fvertexAx..fvertexCy); "SST-1 automatically converts both the fvertex and vertex registers
into an internal fixed point notation used for rendering" [1] §5.2 p. 25. "There are three
vertices in an SST-1 triangle, with the AB and BC edges defining the minor edge and the AC edge
defining the major edge" [1] §5.2 p. 25. The per-vertex parameters and their formats, one
register pair (fixed/float) per component [1] §5.3–5.6 pp. 26–27:

| Component | Fixed-point registers | Format | Owner |
|---|---|---|---|
| Color RGBA | startR/G/B/A, dRdX…dAdY | 12.12 two's complement | FBI |
| Depth Z | startZ, dZdX, dZdY | 20.12 two's complement | FBI |
| Texture S, T | startS/T, dSdX…dTdY | 14.18 two's complement | TREX |
| Perspective W | startW, dWdX, dWdY | 2.30 two's complement | FBI+TREX |

The texture contract is the one that surprises: "the S and T coordinates used by SST-1 for
rendering must be divided by W prior to being sent to SST-1 (i.e. SST-1 iterates S/W and T/W
prior to perspective correction)", and likewise "the W value used by SST-1 for rendering is
actually the reciprocal of the 3D-geometry-calculated W value (i.e. SST-1 iterates 1/W prior to
perspective correction)" [1] §5.5–5.6 p. 26–27. The d?dX registers are added while walking
left-to-right and subtracted right-to-left; d?dY are added walking in +Y and subtracted in −Y
[1] §5.7–5.14 pp. 27–29. The floating-point equivalents exist for every register (fstart*,
fd?dX/Y at 0x0A0–0x0FC) and carry the same semantics in IEEE format [1] §5.7–5.14. Note what
is *not* here: no triangle strip or fan registers, and no setup engine — the host computes and
downloads every start and slope (the setup engine arrives with the second generation,
[voodoo2.md §2.6).

### 2.7 The command registers

**triangleCMD / ftriangleCMD (0x080 / 0x100)** — "Writes to triangleCMD or ftriangleCMD
initiate rendering a triangle defined by the vertex, start, d?dX, and d?dY registers", which
must be set up first [1] §5.15 p. 30. The value written is the *area* of the triangle, and only
its sign matters: bit 31 = 0 means counter-clockwise (positive area), bit 31 = 1 clockwise
(negative); bits 30:0 are ignored [1] §5.15 p. 30. The area is computed by sorting the vertices
by increasing Y and evaluating `AREA = ((dxAB * dyBC) - (dxBC * dyAB)) / 2` [1] §5.15 p. 30.
Three caveats come attached [1] §5.15.1 p. 30:

1. **Wrong sign = hang.** "If the sign of the value sent to the triangleCMD and ftriangleCMD
   registers is not the same as the triangle stored in the vertex registers, FBI will go into
   an infinite rendering loop."
2. **Chip-field transitions can be lost.** "Under certain circumstances, FBI can lose chip
   field changes in successive writes. This often occurs when sending data to the chip for
   triangle rendering. The solution is to write one DWORD of data to the lowest texture address
   in TMU number 3" — a chip select that can never exist in a three-TMU system, so the write is
   a harmless pipeline flush.
3. **Write combining.** "In situations where out-of-order I/O is possible, a fencing operation
   of some sort must occur both before and after the triangleCMD register is written."

**nopCMD (0x120)** — "Writing any data to the nopCMD register executes the NOP command,"
which flushes the graphics pipeline; "the lsb of the data value written to nopCMD... will
clear the fbiPixelsIn, fbiChromaFail, fbiZfuncFail, fbiAfuncFail, and fbiPixelsOut registers"
[1] §5.22 p. 54.

**fastfillCMD (0x124)** — "Writing any data to the fastfill register executes the FASTFILL
command", which clears the RGB and depth buffers as fast as possible: the clip registers are
pre-loaded with the target rectangle (inclusive of left/low, exclusive of right/high), the
color comes from color1, the depth from zaColor bits 15:0, the target buffers from the fbzMode
write-mask bits 10:9 and the draw-buffer field 15:14, and dithering is optional per fbzMode
bit 8 [1] §5.23 p. 54, §6.21 p. 76. In the configuration-space command description: "the
depth-buffer comparison, alpha test, alpha blending, and all other special effects are bypassed
and disabled" during a FASTFILL [1] §6.21 p. 76.

**swapbufferCMD (0x128)** — bit 0 synchronizes the swap to vertical retrace ("If frame buffer
swapping is not synchronized with vertical retrace, then visible frame 'tearing' may occur");
bits 8:1 set a swap interval, a number of retraces to wait before swapping, used "to maintain
constant frame rate"; the internal retrace counter is cleared after each swap, and the
interval is ignored when bit 0 = 0 [1] §5.24 p. 55. The register write is itself the command,
queued in the FIFO like any other (§3.9).

### 2.8 The rendering state: fbzColorPath, fogMode, alphaMode

**fbzColorPath (0x104)** "controls the color and alpha rendering pixel pipelines" — the Color
Combine Unit (CCU) and Alpha Combine Unit (ACU) that produce each pixel's color and alpha from
iterated values, texture output and constants [1] §5.16 pp. 31–36:

| Bits | Meaning |
|---|---|
| 1:0 | RGB select: 0 = iterated RGB, 1 = TREX color output, 2 = color1 RGB |
| 3:2 | alpha select: 0 = iterated A, 1 = TREX alpha output, 2 = color1 alpha |
| 4 | cc_localselect: 0 = iterated RGB, 1 = color0 RGB |
| 6:5 | cca_localselect: 0 = iterated alpha, 1 = color0 alpha, 2 = iterated Z, 3 = reserved |
| 7 | cc_localselect_override: 1 = use texture alpha bit 7 instead of bit 4 |
| 8–16 | CCU controls: zero_other (8), sub_clocal (9), mux select (12:10: 0 = zero, 1 = c_local, 2 = a_other, 3 = a_local, 4 = texture alpha, 5–7 = reserved), reverse blend (13), add_clocal (14), add_alocal (15), invert output (16) |
| 17–25 | ACU controls: zero_other (17), sub_clocal (18), mux select (21:19: same encoding), reverse blend (22), add_clocal (23), add_alocal (24), invert output (25) |
| 26 | parameter adjust: enable sub-pixel correction (§3.3) |
| 27 | enable texture mapping (1 = data transferred from TREX to FBI) |

Two rules bind the texture-enable bit: "If texture-mapped rendering is desired, then bit(27)
of fbzColorPath must be set... If texture mapping is not desired (i.e. Gouraud shading, flat
shading, etc.), then bit(27) may be cleared and no data is transferred from TREX to FBI" — and
"**The nopCMD register must be written before any write to fbzColorPath that changes the state
of Bit(27)**" [1] §5.16 pp. 35–36.

**fogMode (0x108)** "controls the fog functionality" with six bits: bit 0 enables fog;
bits 5:1 are the fog unit's input muxes (fogadd: 0 = fogColor, 1 = zero; fogmult: 0 = CCU RGB,
1 = zero; fogalpha: 0 = fog-table alpha, 1 = iterated alpha; fogz: 0 = fogalpha mux, 1 =
iterated Z bits 27:20; fogconstant: 0 = multiplier output, 1 = fogColor) [1] §5.17 pp. 37–38.
The blend equations for the bit(2:1) combinations are tabulated (`Cout = Afog*Cfog +
(1-Afog)*Cin` for the normal case, with zero, passthrough and blackout variants); bit 3
substitutes the iterated alpha for the fog-table alpha, bit 4 the upper 8 bits of iterated Z
(bit 4 wins if both are set), and bit 5 "takes precedence over bits(4:3) and enables a constant
value (fogColor) to be added to incoming source color" [1] §5.17 pp. 37–38. The fog value
itself is a 64-entry table lookup on the iterated 1/W (§2.11, §3.2).

**alphaMode (0x10C)** "controls the alpha blending and anti-aliasing functionality" [1]
§5.18 p. 39:

| Bits | Meaning |
|---|---|
| 0 | enable alpha test function |
| 3:1 | alpha comparison operator (never / < / = / ≤ / > / ≠ / ≥ / always) |
| 4 | enable alpha blending |
| 5 | enable anti-aliasing — "currently not implemented in SST-1" |
| 11:8 | source RGB blending factor |
| 15:12 | destination RGB blending factor |
| 19:16 | source alpha-channel blending factor |
| 23:20 | destination alpha-channel blending factor |
| 31:24 | alpha reference value |

The blending factors are one of nine values — AZERO, ASRC_ALPHA, A_COLOR (the *other* pixel's
color), ADST_ALPHA, AONE, 1−source alpha, 1−color, 1−destination alpha — plus a dual-valued
0xF: as a source factor it is ASATURATE, `MIN(Source alpha, 1 − Destination alpha)`, "this MIN
function performs polygonal anti-aliasing for polygons which are drawn front-to-back"; as a
destination factor it is the unfogged source color, "useful for multi-pass rendering with
atmospheric effects" [1] §5.18.2 pp. 41–42. A silicon limitation is stated in a footnote:
"the first silicon spin of SST-1 only supports AZERO and AONE for the alpha blending functions
for the alpha channel. All alpha blending functions for the RGB color channels are supported
in the first silicon spin" [1] §5.18.2 p. 42. If the alpha test fails, the pixel is
invalidated and fbiAfuncFail increments [1] §5.18.1 pp. 40–41.

### 2.9 fbzMode, the clip registers and stipple

**fbzMode (0x110)** "controls frame buffer and depth buffer rendering functions... Bits in
fbzMode control clipping, chroma-keying, depth-buffering, dithering, and masking" [1] §5.19
pp. 42–46:

| Bits | Meaning |
|---|---|
| 0 | enable clipping rectangle |
| 1 | enable chroma-keying |
| 2 | enable stipple register masking |
| 3 | W-buffer select (0 = Z for depth, 1 = W for depth) |
| 4 | enable depth-buffering |
| 7:5 | depth comparison operator (never / < / = / ≤ / > / ≠ / ≥ / always) |
| 8 | enable 24→16-bit color dithering (0 = truncation instead) |
| 9 | RGB buffer write mask (0 = invalidate RGB writes) |
| 10 | depth/alpha buffer write mask (0 = invalidate depth writes) |
| 11 | dither algorithm (0 = 4×4 ordered, 1 = 2×2 ordered) |
| 12 | stipple mode when bit 2 is set (0 = rotate mode, 1 = pattern mode) |
| 13 | enable alpha-channel mask (alpha bit 0 = 0 invalidates the pixel) |
| 15:14 | draw buffer (0 = front, 1 = back, 2–3 = reserved) |
| 16 | enable depth biasing (add zaColor bits 15:0 to the calculated depth) |
| 17 | Y origin for rendering operations and pipelined LFB writes (0 = top, 1 = bottom) |
| 18 | enable destination alpha planes (aux buffer as alpha; depth-buffering must then be off) |
| 19 | enable alpha-blending dither subtraction |
| 20 | depth-buffer source compare select (1 = use zaColor[15:0] as the comparison source) — *not implemented in FBI revision 1.0* |

The depth-buffering contract [1] §5.19.1 pp. 46: when enabled, `DEPTHsrc DepthOP DEPTHdst` is
evaluated per pixel; the source pixel reaches the RGB buffer if the comparison is true and
bit 9 is set, and the source depth is written if the comparison is true and bit 10 is set. The
W-buffer reading of bit 3: "the inverse of the normalized w iterator is used for the
depth-buffer comparison. This in effect implements a floating-point w-buffering scheme
utilizing a 4-bit exponent and a 12-bit mantissa. The inverted w iterator is used so that the
same depth buffer comparisons can be used as with a typical z-buffer" [1] §5.19 p. 45. Bit 17
has precedence rules worth quoting whole: it "is used to define the origin of the Y coordinate
for rendering operations (FASTFILL and TRIANGLE commands) and linear frame buffer writes when
the pixel pipeline is bypassed... Note that bit(17) of fbzMode does not affect linear frame
buffer writes when the pixel pipeline is bypassed for linear frame buffer writes (lfbMode
bit(8)=0), as in this situation bit(13) of lfbMode specifies the Y origin for linear frame
buffer writes" — and LFB *reads* are always governed by lfbMode bit 13 [1] §5.19 p. 45, §5.20
p. 52.

**Stipple masking** has two modes, both defined in the fbzMode description [1] §5.19
pp. 43–44. In *rotate* mode (bit 2 set, bit 12 clear), stipple bit 31 masks the current pixel
(0 invalidates it) and "the stipple register is rotated from right-to-left, with the value of
bit(0) filled with the value of bit(31)" after every pixel — and "the stipple register is
rotated regardless of whether stipple masking is enabled... when in stipple rotate mode". In
*pattern* mode (bit 12 set) the register holds a 4×8 pattern selected by the pixel's spatial
coordinates: `pixel_Y[1:0]` picks one of the register's four bytes and `pixel_X[2:0]` picks the
bit within it; the register is never rotated in this mode [1] §5.19 p. 44.

**clipLeftRight (0x118) / clipLowYHighY (0x11C)** — "The clip registers specify a rectangle
within which all drawing operations are confined... The values in the clipping registers are
given in pixel units, and the valid drawing rectangle is inclusive of the clipLeft and
clipLowY register values, but exclusive of the clipRight and clipHighY register values.
clipLowY must be less than clipHighY, and clipLeft must be less than clipRight" [1] §5.21
p. 53. The fields: clipLeftRight carries right in bits 9:0 and left in bits 25:16;
clipLowYHighY carries high Y in bits 9:0 and low Y in bits 25:16 [1] §5.21 p. 53. Two traps:
"when clipping is enabled, the bounding clipping rectangle must always be less than or equal
to the screen resolution", and if clipping is off, "rendering must not be specified to occur
outside of the screen resolution" [1] §5.21 p. 53, [1] §5.19 p. 43. And the Y-origin
interaction: "The clipLowYHighY register is defined such that y=0 always resides at the top of
the monitor screen. Changing the value of the Y origin bits... has no affect on the
clipLowYHighY register orientation" — with a bottom-of-screen origin the software must compute
`scanlines − desired clip value` itself [1] §5.21 p. 53.

### 2.10 lfbMode

**lfbMode (0x114)** "controls linear frame buffer accesses" [1] §5.20 pp. 46–53:

| Bits | Meaning |
|---|---|
| 3:0 | linear frame buffer write format (table below) |
| 5:4 | write buffer select (0 = front, 1 = back, 2–3 = reserved) |
| 7:6 | read buffer select (0 = front, 1 = back, 2 = depth/alpha buffer, 3 = reserved) |
| 8 | pass LFB writes through the SST-1 pixel pipeline (1 = enable) |
| 10:9 | RGBA lane order for LFB data (0 = ARGB, 1 = ABGR, 2 = RGBA, 3 = BGRA) |
| 11 | 16-bit word swap, LFB writes |
| 12 | byte swizzle, LFB writes ("big endian CPUs (e.g. PowerPC processors) should enable byte swizzling") |
| 13 | Y origin for LFB reads and for pipeline-bypassed LFB writes (0 = top, 1 = bottom) |
| 14 | W select for pipelined LFB writes (0 = from the LFB data, 1 = zaColor[15:0]) |
| 15 | 16-bit word swap, LFB reads |
| 16 | byte swizzle, LFB reads |

The write formats [1] §5.20 p. 47:

| Value | Format |
|---|---|
| 0 | 16-bit RGB 5-6-5 |
| 1 | 16-bit RGB x-5-5-5 |
| 2 | 16-bit ARGB 1-5-5-5 |
| 3 | reserved |
| 4 | 24-bit RGB x-8-8-8 (32-bit aligned; packed 24-bit writes not supported) |
| 5 | 32-bit ARGB 8-8-8-8 |
| 6–11 | reserved |
| 12 | 16-bit depth + 16-bit RGB 5-6-5 |
| 13 | 16-bit depth + 16-bit RGB x-5-5-5 |
| 14 | 16-bit depth + 16-bit ARGB 1-5-5-5 |
| 15 | 16-bit depth + 16-bit depth |

The pipeline bit is the fork in behavior [1] §5.20 pp. 47–48: with bit 8 = 1, "LFB pixels are
processed by the normal SST-1 pixel pipeline — this implies each pixel written must have an
associated depth and alpha value, and is also subject to the fog mode, alpha function, etc.",
with missing depth/alpha taken from zaColor; with bit 8 = 0, pixels "bypass the normal SST-1
pixel pipeline and are written to the specified buffer unconditionally", depth function, alpha
blending, alpha test and the color/depth write masks are all skipped, and only the buffers the
chosen format actually names are touched (formats 0–5: color only; 12–14: color and depth;
15: depth only; alpha components land in the alpha buffer only if alpha planes are enabled).
The stipple rotate of §2.9 also runs on pipelined LFB writes.

For the swizzle/swap machinery the order is stated as a rule: "byte swizzling is performed
first on all incoming LFB data... After byte swizzling, 16-bit word swapping is performed...
Finally, after both swizzling and 16-bit word swapping are performed, the individual color
channels are selected" [1] §5.20 p. 51 — and bit 12 "has no affect on linear frame buffer
reads" (reads use bits 15/16 instead, with the operations applied word-swap first, byte-swizzle
second) [1] §5.20.2 pp. 52–53. Bit 11 also fixes the packing of paired 16-bit values inside a
32-bit write (which halfword is the left pixel — or, for formats 12–15, which half is depth)
[1] §5.20 pp. 50–51.

One inconsistency is inside the specification itself and is worth flagging now: the lfbMode bit
table puts the RGBA lane field at bits 10:9 [1] §5.20 p. 47, but the read/write prose twice
describes it as "lfbMode bits(12:9)" [1] §5.20.2 p. 52, §7.1 p. 78 — an impossible four-bit
field that would collide with the word-swap and swizzle bits. The one-bit table is the
authoritative reading; the Linux driver's masks agree with it [6] `sstfb.h` (lanes within
bits 10:9, word swap 11, byte swizzle 12).

### 2.11 Constants, counters and the fog table

| Register | Addr | Bits | Role |
|---|---|---|---|
| fogColor | 0x12C | 23:0 | fog color, RGB in bits 23:16/15:8/7:0 |
| zaColor | 0x130 | 31:0 | constant depth in bits 15:0, constant alpha in bits 31:24 — feeds FASTFILL, depth biasing, pipelined LFB writes, fbzMode bit 20 comparisons [1] §5.26 p. 55 |
| chromaKey | 0x134 | 23:0 | chroma-key color; "the alpha color component of an outgoing pixel is ignored in the chroma-key color match circuitry", and the compare sits "immediately after texture lookup, but before lighting, fog, or alpha blending" [1] §5.27 p. 56 |
| stipple | 0x140 | 31:0 | the stipple mask/pattern of §2.9 |
| color0 | 0x144 | 31:0 | c_local / a_local source for the combine units [1] §5.29 p. 56 |
| color1 | 0x148 | 31:0 | c_other / a_other source; the FASTFILL clear color; the RGB source for pipelined format-15 writes [1] §5.30 p. 56 |
| fbiPixelsIn | 0x14C | 23:0 | pixels processed by the triangle walker, drawn or not [1] §5.31 p. 57 |
| fbiChromaFail | 0x150 | 23:0 | pixels invalidated by the chroma-key test [1] §5.32 p. 57 |
| fbiZfuncFail | 0x154 | 23:0 | pixels failed the depth test [1] §5.33 p. 57 |
| fbiAfuncFail | 0x158 | 23:0 | pixels failed the alpha test or alpha-channel mask [1] §5.34 p. 57 |
| fbiPixelsOut | 0x15C | 23:0 | pixels actually written to a color buffer (RGB mask ignored for the count) [1] §5.35 p. 58 |
| fogTable | 0x160–0x1DC | 31:0 × 32 | the 64-entry fog table [1] §5.36 p. 58 |

All five counters "are reset to 0x0 on power-up reset, and are reset when a '1' is written to
the lsb of nopCMD" [1] §5.31–5.35 pp. 57–58. The fog table packs two entries per word —
`fogTable[n]` carries entry 2n's Δfog in bits 7:0, entry 2n's fog factor in bits 15:8, entry
2n+1's Δfog in bits 23:16 and its fog factor in bits 31:24 — with the fog factors in 8.0
format and the Δ values in 6.2, so "the difference between successive fog blending factors
cannot exceed 63" and "the sum of each fog blending factor and Δfog blending factor pair must
not exceed 255"; loading the whole table takes 32 32-bit writes [1] §5.36 p. 58. At render
time the fog unit indexes the table with the floating-point 1/W (§3.2).

### 2.12 Video timing, the CLUT and the external DAC

The video backend's registers [1] §5.37–5.41 pp. 58–59:

| Register | Addr | Fields |
|---|---|---|
| vRetrace | 0x204 | bits 11:0: the internal vSync_off counter, read only — "allows an application to determine the amount of time before the next vertical sync" |
| hSync | 0x220 | hSync_on in bits 7:0, hSync_off in bits 25:16 |
| vSync | 0x224 | vSync_on in bits 11:0, vSync_off in bits 27:16 |
| backPorch | 0x208 | hBackPorch in bits 7:0, vBackPorch in bits 23:16 |
| videoDimensions | 0x20C | X width in bits 9:0, Y height in bits 25:16 |

The timing algebra is the specification's own [1] §10 pp. 86–87: hSync_on = (number of VCLKs of
active horizontal sync) − 1 and hSync_off = (inactive count) − 1, in units of the video dot
clock; vSync_on and vSync_off are in whole scanlines; hBackPorch = (active horizontal back
porch blank VCLKs) − 2; vBackPorch is in scanlines; the front porches are inferred from sync
and resolution. The vertical interrupt design (never implemented, §3.7) compared the internal
vSync_off counter against a programmable value, which is why the counter is exposed read-only
in vRetrace [1] §10 p. 87.

**clutData (0x228)** loads the internal gamma-correction CLUT: "The Color Lookup Table is
stored internally as a 33x24 RAM. As RGB values are input from memory, the 5 MSBs of a
particular color channel are used to index into the Color Lookup Table. The 3 LSBs of a
particular color channel are then used to linearly interpolate between multiple Color Lookup
Table entries" [1] §5.47 p. 62 — a 33-entry piecewise-linear gamma curve per channel. The
write format carries the index in bits 29:24 (range 0–32) and blue/green/red in bits 7:0,
15:8, 23:16; and "the software video reset bit must be disabled (fbiinit1(8)=0)" or the
write is ignored [1] §5.47 p. 62, §9.5 p. 85.

**dacData (0x22C)** is the port to the card's external RAMDAC: bits 7:0 are the register data,
bits 10:8 the DAC register address, and bit 11 the direction (0 = write, 1 = read) [1] §5.48
p. 63. The bus discipline is the Mac-relevant part: "Writes to the external DAC are only
allowed when the memory bus is idle, as the external DAC register bus is time-multiplexed with
the DRAM data lines. Thus, software must guarantee that there are no conflicts between the
DRAM memory controller and external DAC accesses. This can be accomplished either by holding
the video control unit in reset (fbiinit1 bit(8)=1) and flushing the pixel pipeline with a NOP
command, or by waiting for VSYNC to be active and also flushing the pixel pipeline" [1] §5.48
p. 63. DAC reads are two-step: write dacData with bit 11 set, then read the result from
fbiInit2 bits 7:0 with the fbiInit2/fbiInit3 address remap enabled (initEnable bit 2) — "the
fbiinit2 bits (31:8) are undefined when address remapping is enabled" [1] §5.48 p. 63. The DAC
interface itself is deliberately generic: "SST-1 is compatible with industry standard RAMDACs
and DACs. The DAC interface is identical to that provided by popular graphics accelerators such
as the S3 864 and the Tseng Labs W32p" [1] §3.3 p. 16 — which is why the driver, not the
chipset, decides which DAC part is fitted (§4.2).

**maxRgbDelta (0x230)** holds per-channel thresholds for the video edge-detection filter
("Maximum blue/green/red delta for video filtering" in bits 7:0/15:8/23:16), enabled by
fbiInit1 bit 25; the specification's own description body is "TO BE COMPLETED" [1] §5.49
p. 63 — only the bit field survives.

### 2.13 fbiInit0 through fbiInit4

The five FBI initialization registers share one access contract: "Writes to fbiInit0 are
ignored unless PCI configuration register initWrEnable bit(0)=1. Writes to fbiInit0 are not
put into the PCI bus FIFO and are written immediately, so care must be taken when writing to
fbiInit0 if data is in the PCI bus FIFO or the graphics engine is busy" [1] §5.42 p. 59 —
word-for-word the same for fbiInit1 through fbiInit4 [1] §5.43–5.46 pp. 60–62.

**fbiInit0 (0x210)** [1] §5.42 p. 59:

| Bits | Meaning | Default |
|---|---|---|
| 0 | VGA passthrough (drives the vga_pass / vga_pass_n pins, §3.8) | strap fb_addr[4] |
| 1 | FBI graphics reset (1 = reset) | 0 |
| 2 | FBI FIFO reset — "resets PCI FIFO and the PCI data packer" (1 = reset) | 0 |
| 3 | byte swizzle incoming register writes — "Register byte data is swizzled if fbiInit0[3]==1 and pci_address[21]==1" | 0 |
| 4 | stall PCI on FIFO high-water mark | 1 |
| 10:6 | PCI FIFO empty-entries low water mark (0–31) | 0x10 |
| 11 | route LFB accesses to the memory FIFO | 0 |
| 12 | route texture accesses to the memory FIFO | 0 |
| 13 | memory FIFO enable | 0 |
| 24:14 | memory FIFO high water mark (bits 15:5 of the value) | 0 |
| 30:25 | memory FIFO write burst high water mark (must exceed fbiInit4 bits 7:2) | 0 |

**fbiInit1 (0x214)** [1] §5.43 p. 60:

| Bits | Meaning | Default |
|---|---|---|
| 0 | PCI device function number, strap-latched: 0 = pass-through-only board, 1 = combo board with VGA as device 0 and SST-1 as device 1 (read only) | strap |
| 1 | PCI write wait states (0 = none, 1 = one) | 1 |
| 2 | multi-SST configuration detect (read only) | — |
| 3 | enable linear frame buffer reads — "included so that SST-1 potentially won't hang the system during random reads during powerup" | 0 |
| 7:4 | number of 64×16 video tiles in X, divided by 2 (3.1 format) | 0 |
| 8 | video timing reset (1 = reset) | 1 |
| 9–11 | software override of HSYNC/VSYNC (enable and values) | 0 |
| 12 | software blanking (1 = always blank) | 1 |
| 13–16 | drive-enable (output-enable) bits for video data, blank, HSYNC/VSYNC and DCLK outputs (0 = tristate) | 0 |
| 17 | vclk input select (0 = vid_clk_2x, 1 = vid_clk_slave) | 0 |
| 19:18 | vid_clk_2x delay (0 / 4 / 6 / 8 ns) | 0 |
| 21:20 | video timing vclk source (0 = slave, 1 = vid_clk_2x ÷ 2, 2–3 = vid_clk_2x_sel) | 2 |
| 22 | enable 24-bit-per-pixel video output | 0 |
| 23 | enable scanline interleaving (§3.10) | 0 |
| 25–26 | video edge-detection filter enable; invert vid_clk_2x | 0 |
| 27–30 | vclk / vid_clk_2x_sel delay selects (0 / 4 / 6 / 8 ns) | 0 |
| 31 | disable fast read-ahead write→read turnaround | 0 |

**fbiInit2 (0x218)** [1] §5.44 p. 61 — the DRAM controller: video dither subtraction (bit 0);
DRAM banking configuration (bit 1: 0 = 128K×16, 1 = 256K×16); triple buffering enable (bit 4);
fast RAS read cycles (bit 5); generated DRAM OE (bit 6, default 1); fast read-ahead-write
turnaround (bit 7, requires bit 6); pass-through dither mode "for 8 BPP apps only" (bit 8);
the swap-buffer algorithm (bits 10:9: 0 = on dac_vsync, 1 = on dac_data(0), 2 = on
pci_fifo_stall, 3 = reserved); the video buffer offset in tile rows, "=150 for 640x480, =297
for 832x608" (bits 19:11); DRAM banking enable (bit 20); DRAM read-ahead FIFO (bit 21); DRAM
refresh enable (bit 22) and the 14-bit refresh load value (bits 31:23, internal 5 LSBs zero,
default 0x100).

**fbiInit3 (0x21C)** [1] §5.45 p. 61:

| Bits | Meaning | Default |
|---|---|---|
| 0 | triangle register address remapping (§2.4) | 0 |
| 5:1 | video FIFO threshold | 0 |
| 6 | disable texture mapping (disconnect the TREX-to-FBI interface — the hung-TMU containment bit) | 0 |
| 10:8 | FBI memory type, read only (000 = EDO DRAM, 001 = synchronous DRAM) | strap |
| 11 | VGA_PASS reset value, read only | strap |
| 12 | hardcoded PCI base 0x1F000000, read only (1 = enabled) | strap |
| 16:13 | FBI-to-TREX bus clock delay (0–15) | 0x2 |
| 21:17 | TREX-to-FBI bus FIFO full threshold (0–31) | 0xF |
| 31:22 | Y-origin swap subtraction value (10 bits) | 0 |

**fbiInit4 (0x200)** [1] §5.46 p. 62 — PCI read wait states (bit 0: 0 = one wait state, 1 =
two; default 1); LFB read-ahead logic enable (bit 1); the memory-FIFO low water mark that
dumps the PCI FIFO to memory (bits 7:2); the memory FIFO row start (bits 17:8) and row rollover
(bits 27:18) — the framebuffer rows the memory FIFO occupies.

### 2.14 The TREX register file

The TREX (Texelfx) registers are reached through the same 4 MB register window with the chip
field selecting the TREX (§2.2); reads of all of them are undefined because reads always come
from FBI [1] §5 pp. 18–19.

**textureMode (0x300)** "controls texture mapping functionality including perspective
correction, texture filtering, texture clamping, and multiple texture blending" [1] §5.50
pp. 63–66:

| Bits | Name | Meaning |
|---|---|---|
| 0 | tpersp_st | perspective correction for S/T iterators (0 = linear, force W to 1.0; 1 = perspective-correct) |
| 1 | tminfilter | minification filter (0 = point-sampled, 1 = bilinear) |
| 2 | tmagfilter | magnification filter (0 = point-sampled, 1 = bilinear) |
| 3 | tclampw | clamp when W negative: force S = T = 0 (for projected textures) |
| 4 | tloddither | LOD dither — "adds an average of 3/8 (.375) to the LOD value", to be compensated in lodbias |
| 5 | tnccselect | NCC table select (0 = table 0, 1 = table 1) |
| 6–7 | tclamps / tclampt | clamp the S/T iterators to [0, size) instead of wrapping by truncation |
| 11:8 | tformat | texture format, table below |
| 12–20 | tc_* | texture color combine unit: zero_other, sub_clocal, mux select (14:12: 0 = zero, 1 = c_local, 2 = a_other, 3 = a_local, 4 = LOD, 5 = LOD_frac, 6–7 = reserved), reverse blend, add_clocal, add_alocal, invert output |
| 21–29 | tca_* | texture alpha combine unit, same field set shifted by 9 |
| 30 | trilinear | enable trilinear filtering (0 = point/bilinear) |
| 31 | seq_8_downld | sequential 8-bit download addressing (must be 0 on revision-0 TMUs) |

The texture formats and their expansion to 32-bit ARGB [1] §5.50 pp. 64–65: 8-bit RGB(3-3-2),
8-bit YIQ(4-2-2) (through the NCC tables), 8-bit alpha, 8-bit intensity, 8-bit alpha-intensity
(4-4), 8-bit palette (revision-1 TMU only), 16-bit ARGB(8-3-3-2), 16-bit AYIQ(8-4-2-2),
16-bit RGB(5-6-5), 16-bit ARGB(1-5-5-5), 16-bit ARGB(4-4-4-4), 16-bit alpha-intensity(8-8) and
16-bit alpha+palette (8-8, revision-1 TMU only); values 6–7 and 15 are reserved. There are
"three Texture Color Combine Units (RGB) and one Texture Alpha Combine Unit (A)" — the tc_*
and tca_* fields drive them to blend the two TREXs' outputs (or main and detail texture) in
single- or two-pass fashion [1] §5.50 p. 65.

**tLOD (0x304)** controls the LOD calculation [1] §5.51 pp. 66–68: lodmin (bits 5:0, 4.2
unsigned), lodmax (bits 11:6), lodbias (bits 17:12, 4.2 signed), lod_odd (bit 18), lod_tsplit
(bit 19: odd/even LOD levels split across two TREXs, as selected by lod_odd), lod_s_is_wider
(bit 20), lod_aspect (bits 22:21: 2^n ratio, three rectilinear encodings), lod_zerofrac
(bit 23: force the LOD fraction to zero, for bilinear when even/odd levels are split across
two TREXs), tmultibaseaddr (bit 24: use the texBaseAddr1/2/3_8 split), **tdata_swizzle**
(bit 25: byte-swap incoming texture data) and **tdata_swap** (bit 26: short-swap, performed
after the swizzle) — the texture-download endianness pair, byte swizzle first, short swap
second [1] §5.51 p. 67 — and tdirect_write (bit 27: raw direct texture memory writes, requires
seq_8_downld = 0). "lodbias is added to the calculated LOD value, then it is clamped to the
range [lodmin, min(8.0, lodmax)]" [1] §5.51 p. 67.

**tDetail (0x308)** carries the detail-texture parameters: detail_max (bits 7:0, 8.0
unsigned), detail_bias (bits 13:8, 6.0 signed) and detail_scale (bits 16:14), combining as
`detail_factor = max(detail_max, ((detail_bias − LOD) << detail_scale))` for the texture
combine unit's blend between main and detail texture [1] §5.52 p. 68.

**texBaseAddr / _1 / _2 / _3_8 (0x30C–0x318)** "specifies the starting texture memory address
for accessing a texture, at a granularity of 8 bytes", selected per LOD band by tmultibaseaddr
[1] §5.53 p. 68. The address arithmetic, including the below-zero wrap the mechanism allows,
is §3.5.

**trexInit0 / trexInit1 (0x31C / 0x320)** are the TREX's own bring-up registers — and the
specification gives them one line each: "The trexInit0 register is used for hardware
initialization and configuration of the TREX chip(s). TO BE COMPLETED. See TREX spec." [1]
§5.54–5.55 p. 69. The referenced TREX specification is not in the evidence set; what is known
of the registers comes from the drivers' defaults (§4.1) and the revision-2.0 notes: revision-1
TREX "Added proper software reset functionality (trexInit1: reset_FIFOs, reset_graphics)",
"Added two-memory support (trexInit0: mem2)" and palette write timing (trexInit1: palette_del)
[1] §12 p. 89, and the open-sourced init names the fields the default value programs — EDO
memory type, 16-bit data size, page size 9 bits, second RAS bit 18, refresh enabled with a
0x020 load [5] `sst1init.h`.

**nccTable0 / nccTable1 (0x324–0x380)** are the two 12-entry Narrow Channel Compression
tables: four 32-bit words of sixteen Y values (Y0–Yf) then eight 27-bit words of I0–I3 and
Q0–Q3 (nine bits per component) [1] §5.56.1 pp. 69–70. "Undefined MSB's must be written as 0's,
or the writes may be interpreted as palette writes" — because **the same register space loads
the 256-entry palette** of the revision-1 TMU: "The palette is written through the NCC table 0
I and Q register space when the MSB of the register write data is set. The NCC table write is
inhibited", with the palette index encoded in the low bit of the *register address* (even and
odd addresses alias in pairs — "It is recommended that the table be written as 32 sets of 8 so
that PCI bursts can be 8 transfers long") [1] §5.56.2 pp. 70–71. The NCC tables exist in
pairs "so that they can be swapped on a per-triangle basis when performing multi-pass
rendering, thus avoiding a new download of the table" [1] §5.56.1 p. 69.

### 2.15 PCI configuration space

The header is a standard type-0 layout with a 3dfx tail [1] §6 pp. 72–77:

| Config offset | Register | Notes |
|---|---|---|
| 0x00 | Vendor_ID | 0x121A, read only [1] §6.1 p. 72 |
| 0x02 | Device_ID | 0x0001, read only [1] §6.2 p. 72 |
| 0x04 | Command | **bit 1 (memory access enable) is the only writable bit**; "bits 0, 15:2 are read only", all defaulting to 0 [1] §6.3 p. 72 |
| 0x06 | Status | read only, "hardwired to the value 0x0" [1] §6.4 p. 73 |
| 0x08 | Revision_ID | read only; "Value represents the current revision number" [1] §6.5 p. 73 |
| 0x09 | Class_code | read only, default 0x0 — the device claims no class [1] §6.6 p. 73 |
| 0x0C–0x0F | Cache_line_size, Latency_timer, Header_type, BIST | all read only and hardwired to 0 (header type 0, single function; the device never masters) [1] §6.7–6.10 pp. 73–74 |
| 0x10 | memBaseAddr | the one BAR: R/W, default 0xFF000000, compared against PCI address bits 31:24; writing 0xFFFFFF resets it for size probing [1] §6.11 p. 74 |
| 0x14–0x3B | reserved | |
| 0x3C | Interrupt_line | R/W, **default 0x5 (IRQ5)** — a PC-ism; on a Power Mac the value is whatever firmware writes [1] §6.12 p. 74 |
| 0x3D | Interrupt_pin | read only, hardwired to 0x1 = INTA# [1] §6.13 p. 74 |
| 0x3E–0x3F | Min_gnt / Max_lat | read only, hardwired to 0 — no bus mastering [1] §6.14–6.15 p. 75 |
| 0x40 | initEnable | the write-unlock register, table below [1] §6.16 p. 75 |
| 0x44 / 0x48 | busSnoop0 / busSnoop1 | write-only snooping match addresses; "will return 0x0 when read" [1] §6.17 p. 76 |
| 0x4C | cfgStatus | alias of the memory-mapped status register [1] §6.18 p. 76 |
| 0x50–0xFF | reserved | |

**initEnable (0x40)** "controls write access to the fbiinit registers and also controls the FBI
PCI bus snooping functionality" [1] §6.16 p. 75:

| Bit | Meaning | Default |
|---|---|---|
| 0 | enable writes to fbiInit0–fbiInit3 ("By default writes to the hardware initialization registers are not allowed") | 0 |
| 1 | enable writes to the PCI FIFO — "must be set for normal SST-1 operation" | 0 |
| 2 | remap {fbiInit2, fbiInit3} to {dacRead, videoChecksum} — the DAC-read path of §2.12 | 0 |
| 3 | reserved | |
| 4–6 | snoop register 0: enable, memory/IO match type, write/read match type | 0 |
| 7–9 | snoop register 1: same triple | 0 |
| 10 | scanline-interleave PCI bus ownership (0 = SLI master owns the bus, 1 = slave) | 0 |
| 11 | scanline-interleave master/slave determination (0 = master / even scanlines, 1 = slave / odd) | 0 |

busSnoop0/1 implement the reset-time passthrough guard; the mechanism and its rationale are
§3.8's subject [1] §6.17 p. 76. The chapter closes with four command descriptions (NOP,
TRIANGLE, FASTFILL, SWAPBUFF) that restate the memory-mapped commands of §2.7 [1]
§6.19–6.22 pp. 76–77.

## 3. Behaviour

### 3.1 The PCI slave, write posting and the memory FIFO

The host interface is deliberately deep, because a slave-only device lives or dies by write
posting: "SST-1 uses an asynchronous FIFO 64 entries deep which allows sufficient write posting
capabilities for high performance. The FIFO is asynchronous to the graphics engine, thus
allowing the memory interface to operate at maximum frequency regardless of the frequency of
the PCI bus. Zero-wait-state writes are supported for maximum bus bandwidth" [1] §3.3 p. 13.
On top of it, "SST-1 can optionally use off-screen frame buffer memory to increase the
effective depth of the PCI Bus FIFO. The depth of this memory FIFO is programmable, and when
used as an addition to the regular 64 entry host FIFO, allows up to 65536 host writes to be
queued without stalling the PCI interface" [1] §3.3 p. 13 — its enable bits, water marks and
framebuffer row placement are the fbiInit0/fbiInit4 fields of §2.13, and its occupancy is
status bits 27:12. The FIFO discipline that software must respect is the register table's
Sync/FIFO pair (§2.3): synchronized registers flush the pipe before loading, non-FIFOed
registers act immediately and can overtake queued writes, and reads never enter the FIFO [1]
§5 pp. 18–19. LFB reads bypass both FIFOs and block (§3.4).

### 3.2 The pixel pipeline

"The rendering engine is structured as a pipeline through which each pixel drawn to the screen
must pass. The individual stages of the pixel pipeline modify pixels or make decisions about
them" [1] §3.2 p. 12. Glide describes the same diagram in words: "The input to the pixel
pipeline can come from one of four sources: a texture value, an iterated RGBA value, a
constant RGBA value, or data for a frame buffer write. Pixels that pass the chroma-key test go
to the color combine unit where a user-specified lighting function is applied. The special
effects unit further modifies the pixel with alpha and depth testing, fog, and alpha blending
operations. The final 24-bit color value is then dithered to 16 bits and written to the frame
buffer" [2] Ch. 1 pp. 5–6. The fog stage indexes its 64-entry table (§2.11) with the
normalized floating-point 1/W; the specification describes "a 6-bit floating point
representation of (1/W)" used to index the table with "low order bits of the floating point
(1/W)... used to blend between multiple entries of the lookup table to reduce fog banding"
[1] §3.3 p. 15, while Glide states the same mechanism as "a 14-bit floating point
representation of 1/w... used to index into the 64-entry lookup table and interpolate between
entries" [2] Ch. 1 p. 5 — six bits of index and eight bits of interpolation fraction of one
14-bit value, the two descriptions consistent (*inferred*; neither document says so
explicitly). All internal color math is 24-bit; the 16-bit conversion is the back-end dither
of fbzMode bit 8 (§2.9) [1] §3.3 p. 16.

### 3.3 Triangle rendering and sub-pixel correction

SST-1 renders one primitive: "SST-1 supports a triangle drawing primitive — spans (both
horizontal and vertical) and lines are rendered as special case triangles. Complex primitives
such as quadrilaterals must be decomposed into triangles before they can be rendered by
SST-1" [1] §3.3 p. 14. The host owns setup entirely: it downloads the vertices, the starting
parameters and all six slopes per component (§2.6), and the sign of the written area value must
match the vertex winding or the walker hangs (§2.7). Sub-pixel correction "is performed in the
on-chip triangle setup unit of SST-1. When subpixel correction is enabled (fbzColorPath(26)=1),
the incoming starting color, depth, and texture coordinate parameters are all corrected for
non-integer aligned starting triangle <x,y> coordinates... As the starting color, depth, and
texture coordinate parameters are read from the PCI FIFO" [1] §9.4 p. 84 — enabling it drops
the setup unit from 7 to 16 clocks, "but as the triangle setup engine is separately pipelined
from the triangle rasterization engine, little if any performance penalty is seen" [1] §5.16
p. 36. The sticky-parameter rule: "If a triangle is rendered with subpixel correction enabled,
all subsequent triangles must resend starting color, depth, and texture coordinate parameters,
otherwise the last triangle's subpixel corrected starting parameters will be subpixel corrected
(again!), and inaccuracies will result" [1] §9.4 p. 84, [1] §5.16 p. 36.

The idling discipline is a correctness requirement, not a style suggestion: "After certain SST
operations, and specifically after linear frame buffer accesses, there exists a potential
deadlock condition between internal SST state machines... To avoid this problem, always issue
a NOP command before reading the status register when polling on the SST busy bit. Also, to
avoid asynchronous boundary conditions when determining the idle status, always read SST
inactive in status three times" — the specification's own `SST_IDLE()` loop writes nopCMD,
then requires three consecutive not-busy status reads [1] §9.3 p. 84.

### 3.4 Linear framebuffer access

The LFB port occupies the middle 4 MB of the aperture and gives the host direct, mode-formatted
access to the color, depth and alpha buffers: "Regardless of actual frame buffer resolution,
all linear frame buffer accesses assume a 1024-pixel logical scan line width", 2048 bytes per
scanline for 16-bit formats and 4096 for 32-bit [1] §7 p. 78, §7.1 pp. 78–79. Writes take the
lfbMode formats of §2.10; reads are always 16/16 — "all data returned is in 16/16 format, with
two 16-bit pixels returned for every 32-bit doubleword read", their order fixed by lfbMode
bit 15, and the depth/alpha buffer is readable by selecting it in lfbMode bits 7:6 [1]
§5.20.2 p. 52, §7.2 p. 79. Accesses may be 32-bit or, for 16-bit formats, 16-bit; byte
accesses are illegal everywhere (§9.2 p. 84) [1].

Two performance contracts shape driver code. Reads block: "reads from the linear frame buffer
bypass the PCI host FIFO (as well as the memory FIFO if enabled) but are blocking. If the host
FIFO has numerous commands held, then the read will take potentially a very long time before
data is returned, as data is not read from the frame buffer until the PCI host FIFO is empty
and the graphics pixel pipeline has been flushed" — the recommended pattern is to verify idle
via status first [1] §7.2 p. 79. And clipping is not automatic: "Linear frame buffer writes to
areas outside of the monitor resolution when clipping is disabled will result in undefined
behavior" [1] §7 p. 78. Glide surfaces the whole mechanism as `grLfbLock()`/`grLfbUnlock()`:
a lock "will cause the 3D graphics engine to idle", "an application may not call any Glide
routines other than grLfbLock() and grLfbUnlock() while any lock is active", the pixel
pipeline may be enabled per lock with depth/alpha taken from `grLfbConstantDepth` /
`grLfbConstantAlpha`, and ordered writes go through `grLfbWriteRegion()` [2] Ch. 11 pp. 117–132.

### 3.5 Texture memory and the texture pipeline

The texture port is the top 8 MB of the aperture and is **write-only**: "Note that the texture
memory is write only — reading from the texture memory address space returns undefined data"
[1] §8 p. 80. The contents are laid out as if every texture were a full mipmap: "Textures are
stored as if mipmapped, even for textures containing only one level of detail. The largest
texel map (LOD=0) is stored first, and the others are packed contiguously after. texbaseaddr
points to where the texture would start if it contained LOD level 0 (256x* dimension), in a
granularity of 8 bytes" [1] §8 p. 80. The base may legitimately wrap below zero — the examples
show `texbaseaddr = start − size(LOD0..LODmin−1)` going negative with lodmin keeping actual
lookups positive — and "when two memory banks are used (8 DRAMs), a texture can not span both
banks because each bank has one RAS" [1] §8 pp. 80–82. Per-LOD sizes in 8-byte units follow the
aspect-ratio table (a 256×256 16-bit level 0 is 2^14 units, halving per LOD; 8-bit formats halve
that, with half-unit remainders unusable by the next texture) [1] §8 pp. 81–82.

Downloads are addressed, not streamed: the PCI byte address of a texture write carries the TREX
select in bits 22:21, the LOD in bits 20:17, T in bits 16:9 and S in bits 8:2 (S right-aligned
to bit 2 and T to bit 9 for textures smaller than 256 in each dimension; for 8-bit textures the
low S bit is forced to 0), so "Two 16-bit or four 8-bit texels are written at a time", with
inhibited upper bytes for narrow textures [1] §8 pp. 82–83. The revision-1 TMU's seq_8_downld
mode repacks the S field for sequential 8-bit downloads [1] §8 p. 83, [1] §12 p. 89. At render
time the per-pixel divide gives true perspective mapping at no cost: "there is no performance
penalty for performing perspective corrected texture mapping" [1] §5.50 p. 64; LOD dithering
"adds an average of 3/8 to the LOD value" and nearly matches trilinear quality when combined
with bilinear filtering [1] §5.50 p. 64, [1] §3.3 p. 14. Glide's allocator exposes the same
8-byte granularity: `grTexMinAddress()`/`grTexMaxAddress()`, start addresses that "must be
8-byte aligned", and `grTexDownloadMipMap()` with the even/odd level mask — the
`GR_MIPMAPLEVELMASK_EVEN/ODD/BOTH` selector that pairs with tLOD's lod_tsplit [2] Ch. 10
pp. 95–116.

### 3.6 Video output and gamma

The video controller fetches through the tiled framebuffer (§1.5) and drives a generic external
DAC (§2.12) with programmable timing (§2.12). The 16-bit framebuffer values pass the internal
gamma CLUT on the way out: "The 16-bit dithered color data from the frame buffer is used as an
index into the gamma-correction color table — the 24-bit output of the gamma-correction color
table is then fed to the monitor" [1] §3.3 p. 16. A 24-bit-per-pixel video output mode exists
(fbiInit1 bit 22) alongside the 16-bit default [1] §5.43 p. 60. The video FIFO threshold
(fbiInit3 bits 5:1) and the video buffer offset (fbiInit2 bits 19:11, "=150 for 640x480, =297
for 832x608") are the two tuning knobs the mode-set actually programs [1] §5.44–5.45 pp. 61–62.

### 3.7 Interrupts: specified, removed, not implemented

The interrupt path exists on paper and nowhere else. The pin is specified — INTA#, hardwired
[1] §6.13 p. 74 — and the status register reserves bit 31 for "PCI Interrupt Generated", with
the design that "If SST-1 generates a vertical retrace interrupt (as defined in pciInterrupt),
bit(31) is set and the PCI interrupt signal line is activated... An interrupt is cleared by
writing to status with 'dont-care' data"; but the same paragraph ends: "NOTE THAT BIT(31) IS
CURRENTLY NOT IMPLEMENTED IN HARDWARE, AND WILL ALWAYS RETURN 0x0" [1] §5.1 p. 24. The planned
`pciInterrupt` register (whose bits 27:16 would have compared against the vSync_off counter,
[1] §10 p. 87) was deleted from the document at revision 1.20 [1] §13 p. 90. The consequence
for every driver is polling: busy-wait on status bits 6/7/8/9, with the NOP-and-three-reads
discipline of §3.3, and the swap-pending count of §3.9 as the only asynchronous progress
signal. The same conclusion is stated from the outside: the generation "had no interrupt
mechanism, so the driver had to poll" [11]. Contrast the second generation, which adds a real
interrupt controller ([voodoo2.md](voodoo2.md) §3.7) — and whose Mac software, notably, still
never arms it ([voodoo2.md](voodoo2.md) §5).

### 3.8 Passthrough, bus snooping and the video takeover

The vga_pass pins (fbiInit0 bit 0) are the mechanical heart of a pass-through board: the bit
controls "external pins vga_pass and vga_pass_n", and its power-on default is a strap
(fb_addr[4] on first silicon; the second split the strap so fb_addr_a[5] hardcodes the PCI
base and fb_addr_a[4] keeps the vga_pass default, [1] §12 p. 89) [1] §5.42 p. 59. With the bit
clear, the host 2D card's analog output flows through the board's DAC socket to the monitor;
with it set, the SST-1's own video backend drives the monitor. Glide exposes exactly this as
`grSstControlMode(GR_CONTROL_ACTIVATE)` / `GR_CONTROL_DEACTIVATE` — "grSstControlMode
determines whether the VGA display or Voodoo Graphics display is visible" — with the SST-1
note that "since the 2D and 3D graphics exist on different devices (and frame buffers),
activating or deactivating pass through does not require you repaint either the 2D or 3D
graphics", and the note that the call "supersedes the now-obsolete grSstPassthru routine" [3]
§grSstControlMode p. 67.

Two reset-time hazards get dedicated hardware. First, the vga_pass reset value is a strap,
mirrored read-only in fbiInit3 bit 11 (§2.13) — so the board decides, before any software
runs, which signal owns the monitor. Second, the busSnoop registers (§2.15) watch the PCI bus
for a specifically shaped cycle and force vga_pass on when they see one, so that "VGA
passthrough capability does not drive the video monitor upon soft and hard resets" [1] §6.17
p. 76 — the analog reset glitch every pass-through card must survive. The Linux driver keeps
the mechanism as a runtime switch (`vgapass=1`, plus a sysfs toggle) and warns what happens
without it: once the module loads, "the 3dfx takes control of the output" [6] `sstfb.rst`.

### 3.9 Buffer swaps and triple buffering

The swap is a FIFOed command, not a register poke the CPU must time: "When a swapbufferCMD is
received in the front-end PCI host FIFO, the swap buffers pending field in the status register
is incremented... when an actual frame buffer swapping occurs... [it] is decremented", giving
software "a way to determine how many SWAPBUFFER commands are present in the SST-1 FIFOs"
without polling [1] §5.24 p. 55. Synchronized swaps wait for vertical retrace (bit 0), and the
interval field (bits 8:1) defers the swap by a counted number of retraces for frame-rate
governing [1] §5.24 p. 55. Triple buffering exists as a DRAM-controller mode (fbiInit2 bit 4)
with its own rule: "syncing to vertical retrace must be enabled and the swapbuffer interval
must be 0x0 when using triple buffering" [1] §5.24 p. 55; the specification's fuller
description is flagged "TO BE COMPLETED" [1] §5.24 p. 55. Glide maps the whole mechanism onto
`grBufferSwap(interval)` — "it queues the buffer swap command and returns immediately" — and
`grBufferNumPending()`, "The maximum value returned is 7, even though there may be more
buffer swap requests in the queue" [2] Ch. 3 p. 23. The front/back/aux buffer layout — the aux
buffer serving depth, alpha or a third color buffer, mutually exclusive, absent at 800×600 on
a 2 MB board — is Glide's Table 3.3 [2] Ch. 3 pp. 21–23; a clear runs at twice the triangle
fill rate [2] Ch. 3 p. 24.

### 3.10 Scanline interleaving

SLI chains two complete FBI/TREX subsystems on alternate scanlines of one image, doubling the
rendering rate [1] §3.1 pp. 10–11. The specification's own chapter on it is a stub — "This
section to be completed" — but it does list what changes when SLI is enabled: polling status
(cfgStatus included), the Y-origin bit (an even yorigin_swapval is required to swap origins),
linear framebuffer reads (who controls the PCI bus changes), the setup sequence, and flushing
the PCI packer [1] §11 p. 88. The register surface is elsewhere: fbiInit1 bit 23 enables
interleaving, initEnable bits 10–11 decide which board owns the PCI bus and which renders even
versus odd scanlines (§2.15), and the Y-origin swap subtraction value lives in fbiInit3 bits
31:22 [1] §5.43, §5.45, §6.16 pp. 60–75. Glide hides the pair from the application entirely:
"two Voodoo Graphics subsystems in a scanline-interleaved configuration are treated as if a
single Voodoo Graphics subsystem is installed in the system, including during Voodoo Graphics
selection, initialization, state management, texture download, etc." [2] Ch. 3 p. 20. The
retail Mac card is a single board with no companion connector; nothing in the Mac evidence set
exercises SLI (§6.11).

## 4. Programming model

### 4.1 Power-on bring-up

Two independent drivers publish complete SST-1 bring-up sequences: the open-sourced Glide
SST-1 init module [5] and the Linux `sstfb` driver [6]. They agree on the shape, and the
shape is what a re-implementation must reproduce (`sstfb`'s `sst_init` [6] `sstfb.c`):

1. **Find the card** by scanning configuration space for vendor 0x121A / device 0x0001 [5]
   `sst1init.c`; map the 16 MB aperture; set the configuration Command register's
   memory-access bit — the only writable bit the header has (§2.15).
2. **Unlock**: write initEnable = init-write-enable only (bit 0). The FIFO write-enable bit
   stays off until the init registers are programmed [6] `sst_init`, [5].
3. **Reset, in order**: video timing reset (fbiInit1 bit 8) first, then graphics + PCI FIFO
   reset (fbiInit0 bits 1–2), then drop DRAM refresh (fbiInit2 bit 22), idling between
   steps — "the same shape of sequence" the second-generation init performs with the
   justification that video must be reset before graphics or "video unit could potentially
   hang waiting for the graphics unit to respond" ([voodoo2.md](voodoo2.md) §4.1).
4. **Enable the fbiInit2/3 remap** (initEnable bit 2), detect the DAC and program the clocks
   (§4.2).
5. **Write the init-register defaults**, idling between writes. Glide's defaults [5]
   `sst1init.h`: fbiInit0 = 0x00000410 (memory FIFO enabled for LFB and texture, PCI FIFO
   low-water stall on), fbiInit1 = 0x00201102, fbiInit2 = 0x80000040 (refresh on, 16 ms
   load, fast RAS and read-ahead on), fbiInit3 = 0x001E4000 (texture mapping *disabled*, the
   hung-TMU containment bit 6 set, TREX FIFO threshold 0xF, FBI-to-TREX clock delay 0x2),
   fbiInit4 = 0x00000001, plus `trexInit0` per TMU (EDO memory, 16-bit data size, 9-bit
   pages, second RAS bit 18, refresh enabled with 0x020 load). The Linux driver's defaults
   are a framebuffer-console's subset: fbiInit0 = passthrough-disabled (or enabled per the
   `vgapass` option), fbiInit1 = fast PCI writes | video reset | 10 tiles-in-X | vclk select
   | LFB-read enable, fbiInit2 = refresh on, fbiInit3 = texture disabled, fbiInit4 = fast
   reads | LFB read-ahead [6] `sstfb.h` (FBIINIT0–4_DEFAULT).
6. **Enable the FIFO** (initEnable bit 1), then re-enable the video clock and program the
   mode (§4.3).

Shutdown reverses it: reset video/graphics/FIFO, disable refresh, drop the graphics clock to
20 MHz, clear the resets, disable passthrough and the video clock, initEnable = 0 [6]
`sst_shutdown`.

### 4.2 DAC detection and clock programming

SST-1 boards carry an external RAMDAC reachable through dacData (§2.12), and the driver
cannot assume which part is fitted. The Glide init and the Linux driver both probe three
families in order — TI, AT&T, ICS [5] `dac.c`, [6] `sstfb.c` — and the identification is
concrete: AT&T parts identify with MIR = 0x84 and TI with MIR = 0x97 (both DIR = 0x09), while
"the ICS part is identified differently, by reading its PLL registers back and comparing
against the power-on values" (f1 = 0x55, f7 = 0x71, fB = 0x79) [6] `sstfb.h`
(DACREG_MIR_ATT/TI, DACREG_ICS_PLL_CLK0_1/7 and CLK1_B initial values), the same values the
second-generation init uses ([voodoo2.md](voodoo2.md) §4.2). Both video and graphics clocks
are PLL conversations with the DAC: the shared formula is `Fout = Fref × (M+2) / (2^P ×
(N+2))` with the search preferring low M and high P, VCO bounded near 250 MHz [6]
`sst_calc_pll`. The graphics clock default for this generation is 50 MHz — the SST-1 init's
documented environment variable range is "16 ≤ Frequency < 80 (50 is default)" [5]
`sst1init.c`, and the Linux driver caps the override at 60 MHz [6] `sstfb.c`. Which DAC the
Power3D actually carries is not documented by any page in the evidence set (§6.4).

### 4.3 A real mode-set

The Linux driver's `sstfb_set_par` is the cleanest published end-to-end mode-set, and its
Voodoo-1 branch differs from the second generation's in exactly two ways [6] `sstfb.c`:

```
write nopCMD = 0;                    poll status until idle
cfg.initEnable = EN_INIT_WR
set fbiInit1 VIDEO_RESET, fbiInit0 FBI_RESET|FIFO_RESET, clear fbiInit2 EN_DRAM_REFRESH
backPorch     = vBackPorch << 16 | (left_margin - 2)
videoDimensions = yDim << 16 | (xres - 1)
hSync = (hSyncOff - 1) << 16 | (hsync_len - 1)
vSync = vSyncOff << 16 | vSyncOn
cfg.initEnable = EN_INIT_WR | REMAP_DAC ; dac.set_vidmod ; dac.set_pll ; restore
fbiInit1 |= output drives | tiles-in-X | vclk select      (fbiInit6/5 are Voodoo2-only)
release VIDEO_RESET, FBI_RESET, FIFO_RESET; set EN_DRAM_REFRESH
cfg.initEnable = EN_FIFO_WR
lfbMode = 5-6-5 format, front buffer, pipeline off (plus swizzle bits on big-endian hosts)
```

The Voodoo-1 specifics: the tile count is "voodoo1 has 64 pixels wide tiles" — `tiles_in_X =
(xres + 63) / 64`, written whole into fbiInit1 bits 7:4 (the second generation counts
32-pixel tiles in three pieces across fbiInit1/5/6, [voodoo2.md](voodoo2.md) §4.3); the
driver's validity limits encode the SST-1 register field widths — xres ≤ 1024, yres < 1024,
hsync_len ≤ 256, hSyncOff ≤ 2048, vSyncOn < 4096, vSyncOff < 4096, vBackPorch < 256,
tiles_in_X < 16, no interlace or doublescan; and it leaves clipping on by default with the
warning that off-screen LFB writes are "undefined (_very_ undefined)". The driver is
16-bpp-only by choice ("The driver is 16 bpp only, 24/32 won't work", a software limit; the
hardware's unusual 24/32 behavior is noted in the same breath) [6] `sstfb.rst`.

### 4.4 The Glide contract

Glide 2.x is the native API, and the SST-1 register file is essentially Glide's public state
flattened into longwords. The application-side contract [2] Ch. 3 pp. 15–21: call
`grSstQueryBoards()` first — "grSstQueryBoards() is the only Glide routine that can be called
before grGlideInit(); it does not change the state of any hardware, nor does it render any
graphics" — then `grGlideInit()`, `grSstQueryHardware()` (which fills in the board
configuration), `grSstSelect()`, and
`grSstWinOpen(resolution, refresh, color format, origin, numColorBuffers, numAuxBuffers)`;
"failing to do this will cause the system to operate in an undefined (and, most likely,
undesirable) state". `grRenderBuffer` picks the draw buffer; `grBufferClear` issues the
FASTFILL of §2.7; `grBufferSwap(interval)` queues the SWAPBUFFER of §3.9;
`grBufferNumPending` reads the swap-pending field; `grSstVRetraceOn`/`grSstVideoLine` read
the beam position (the vRetrace register of §2.12) [2] Ch. 3 pp. 23–25. The per-subsystem
state rule matters to multi-board code: "Each Voodoo Graphics subsystem has its own version of
the Glide state variables", selected by `grSstSelect()` [2] Ch. 3 p. 19. The color format
argument chooses one of four byte orders for pixels handed to Glide — "for the widest
possible compatibility... Glide provides 'byte swizzling'" [2] Ch. 3 p. 18 — the API-level
front end of the lfbMode lane machinery. Texture management (Ch. 10) is the texbaseaddr
arithmetic of §3.5 behind an allocator; linear framebuffer access (Ch. 11) is lfbMode behind
`grLfbLock`/`grLfbWriteRegion`. "Voodoo Graphics was internally code-named SST-1, or SST for
short. Some of the API names, e.g. grSstWinOpen, still reflect the internal code name" [3]
p. 1.

### 4.5 The 1997 Power3D driver

The Mac driver for this generation is a single release, dated 1997-08-27 — listed on the
3dfxBIOS "VoodooMAC" index as Voodoo Graphics "Drivers only", version unknown, 08/27/1997 [8]
— and it is a dramatically smaller stack than the second generation's (contrast
[voodoo2.md](voodoo2.md) §4.5). The archive `VOODOO_DRV_08-27-1997.zip` wraps a StuffIt
self-extractor (`VOODOO_DRV_08-27-1997.sea`, "StuffIt (c)1997-1998 Aladdin Systems") whose
readable content is small and complete (*observed*, reading the archive's own strings): a
folder named **"Power3D MacOS"** with the install instruction **"Drag files onto System
Folder"**, and exactly two shared libraries:

- **`3Dfx RAVE`** — type/creator `shlb`/`tnsl`, the QuickDraw 3D RAVE drawing-engine shim;
  the archive's strings show the QuickDraw 3D Accelerator library reference (`shlb`/`QD3D`)
  alongside it, matching the import set of the later RAVE shim ([voodoo2.md](voodoo2.md)
  §4.5).
- **`3DfxGlideLib2.x`** — type `shlb`, creator `????` — the Glide 2.x shared library
  itself, a user-space CFM library with no classic device driver behind it.

No `'ndrv'` driver, no interrupt extension, no OpenGL renderer and no Name-Registry-matching
extension of any kind appears in the archive (*observed*; the OpenGL renderer is a
second-generation addition). The whole Mac contract is therefore: two libraries in the
Extensions folder, a RAVE engine for QuickDraw 3D titles, Glide for native titles, and the
machine's built-in video unmodified for everything else. TechWorks's package text confirms
both APIs were sold as features ("Supports QuickDraw 3D RAVE games. Exclusive — native 3Dfx
Glide API support!") [9], and the OS floor for the pair is "MacOS 7.5.3 or greater" [9].

One trap survives the generation gap and is worth stating here because the file names are
read from both archives: the 1997 Voodoo Graphics library registers the CFM fragment
`3DfxGlideLib2.x` from a file *without* a space in its name (*observed*, [7]), while the
1999–2000 Voodoo2 driver set carries a file named `3dfx GlideLib2.x` *with* a space that
registers the *same* fragment name ([voodoo2.md](voodoo2.md) §4.5, §5). With both installed,
CFM can bind an application's Glide imports to the 1997 library — which cannot see a
Voodoo2 — and the failure is silent.

### 4.6 Discovery on the Macintosh

Because the card has no ROM, the Mac software owns discovery end to end, and the mechanism is
the second generation's exactly (documented with its symbol evidence there,
[voodoo2.md](voodoo2.md) §4.6): the user-space Glide library walks the Name Registry itself,
finds the PCI node by vendor/device ID, reads `assigned-addresses` for the aperture, and
performs all configuration-space access through the Expansion Bus Manager rather than raw
memory writes. What Open Firmware contributes is only the enumeration of §1.4: address space
allocated, no FCode, no driver loaded, no name property [4] p. 88 — the node carries the
standard generated PCI identity naming ([pci.md](../pci.md) §2.7–2.8, §4.4). The Mac
library for this generation carries the same init machinery as the PC one — the
second-generation Mac GlideLib's symbol strings expose `MacCheckBoardsInSystem`,
`pciFindCardMulti`, `pciMapCard`, `sst1InitMapBoard`, `sst1InitDacDetect*` and
`.SstSetupMac` ([voodoo2.md](voodoo2.md) §4.6) — and the 1997 library predates it with the
same design (*inferred*; the 1997 archive's binary is not symbol-mapped here, only its file
names and type/creator codes are read, §6.5).

### 4.7 Endianness

The chipset meets a big-endian host with three explicit mechanisms, and the Power Macintosh
needs all of them [1] §5.20 pp. 48–51:

1. **Register accesses**: fbiInit0 bit 3 arms a swizzle that is gated on the *address* —
   "Register byte data is swizzled if fbiInit0[3]==1 and pci_address[21]==1" [1] §5.42 p. 59.
   The gate is the wrap field's MSB (§2.2), so software can swizzle whole bursts by choosing
   the wrap. (The second generation moved the gate to a different address bit;
   [voodoo2.md](voodoo2.md) §4.7.)
2. **Linear framebuffer accesses**: the lfbMode swizzle/swap machinery of §2.10 — bit 12
   byte-swizzles each longword, bit 11 word-swaps the pairs afterward, bits 10:9 pick the
   lanes last, and bits 15/16 mirror both controls for reads — with the design intent stated
   outright: "big endian CPUs (e.g. PowerPC processors) should enable byte swizzling" [1]
   §5.20 pp. 48–53.
3. **Texture downloads**: tLOD bits 25/26 (tdata_swizzle, tdata_swap) reorder each incoming
   texture word, "byte swizzle first and short swap second" [1] §5.51 p. 67.

The Linux driver's big-endian port uses exactly this, and its comment is the clearest
statement of the contract: "Enable byte-swizzle functionality in hardware. With this enabled,
all our read- and write-accesses to the voodoo framebuffer can be done in native format, and
the hardware will automatically convert it to little-endian" [6] `sstfb.c` — the
`__BIG_ENDIAN` branch sets all four LFB bits (word and byte, read and write). The 1997 Mac
library's split between hardware swizzling and software conversion per color format is not
directly evidenced (§6.6); the second-generation Mac library exports both paths
(`grLfbWriteColorSwizzle`, `guEndianSwapBytes`/`guEndianSwapWords`,
[voodoo2.md](voodoo2.md) §4.7).

### 4.8 The configuration variables

The Glide SST-1 init reads a set of `SST_*` environment variables — the escape hatches for
boards that do not match the defaults. The list is the init module's own documented table
[5] `sst1init.c`:

| Variable | Knob |
|---|---|
| SST_FBICFG / SST_TMUCFG | explicit FBI/TMU configuration strapping overrides |
| SST_FBIMEM_SIZE / SST_TMUMEM_SIZE | memory sizes instead of detection |
| SST_GRXCLK | graphics clock, "16 <= Frequency < 80 (50 is default)" |
| SST_SCREENREZ (512/640/800) / SST_SCREENREFRESH (60/75) | video resolution and refresh overrides |
| SST_VIDCLK2X | video clock (2× dot clock) |
| SST_HSYNC, SST_VSYNC, SST_BACKPORCH, SST_DIMENSIONS, SST_TILESINX, SST_MEMOFFSET | raw video register overrides |
| SST_GAMMA, SST_IGNORE_INIT_GAMMA | gamma correction |
| SST_MEMFIFO, SST_MEMFIFO_LWM/HWM/ENTRIES, SST_MEMFIFO_LFB/TEX | the memory FIFO (§3.1) |
| SST_PCIFIFO_LWM | the PCI FIFO low water mark |
| SST_FT_CLK_DEL, SST_PFT_CLK_DEL, SST_PTF0/1/2_CLK_DEL, SST_TF0/1/2_CLK_DEL | FBI↔TREX bus clock delays (preliminary and final) |
| SST_TF_FIFO_THRESH | TREX-to-FBI FIFO threshold |
| SST_TREX0/1/2INIT0, SST_TREX0/1/2INIT1 | per-TMU trexInit register overrides |
| SST_TRIPLE_BUFFER | triple buffering |
| SST_VGA_PASS | force the vga_pass output to 0 or 1 |
| SST_SLIDETECT, SST_SLIM/SLIS_VIN/VOUT_CLKDEL | scanline interleave detection and SLI clock delays |
| SST_VIDEO_24BPP, SST_VIN_CLKDEL, SST_VOUT_CLKDEL | video backend |
| SST_SLOWPCIWR, SST_FASTPCIRD, SST_FASTMEM | PCI and memory timing |
| SST_TEXMAP_DISABLE, SST_NOCHECKHANG, SST_NODEVICEINFO, SST_NOSHUTDOWN, SST_IGNORE_INIT_REGISTERS/VIDEO/GAMMA, SST_IGNORE_SLI_CHECK, SST_INITDEBUG(_FILE) | bring-up and bring-down switches |

The second generation renames the same knobs `SSTV2_*` and reads them on the Mac from a
`voodoo2.var` file ([voodoo2.md](voodoo2.md) §4.8); whether the 1997 Voodoo Graphics Mac
library reads an equivalent `.var`/INI file is not evidenced (§6.5).

## 5. Quirks & errata

- **Wrong area sign hangs the chip.** A triangleCMD whose sign bit disagrees with the vertex
  winding puts FBI "into an infinite rendering loop" [1] §5.15.1.1 p. 30.
- **Chip-field transitions can be lost.** The prescribed fix is a dummy write to "the lowest
  texture address in TMU number 3" — a select that cannot exist [1] §5.15.1.2 p. 30.
- **Sub-pixel correction is sticky.** One triangle with fbzColorPath bit 26 set corrupts the
  next triangle's start parameters unless all of them are re-sent [1] §9.4 p. 84, §5.16
  p. 36.
- **NOP before every status poll, and idle must be read three times.** There is a known
  deadlock window after LFB accesses; the `SST_IDLE()` recipe is a correctness requirement
  [1] §9.3 p. 84.
- **nopCMD before changing fbzColorPath bit 27** — the texture-enable transition requires a
  pipeline flush first [1] §5.16 pp. 35–36.
- **32-bit accesses only, everywhere except 16-bit LFB by format**; the byte field must be
  zero, and bit-twiddling a register means rewriting the whole longword [1] §5 p. 19, §9.2
  p. 84.
- **The non-FIFOed registers are a race.** fbiInit0–4, the video timing registers, clutData,
  dacData and maxRgbDelta act immediately and can overtake queued graphics writes [1]
  §5.42–5.46 pp. 59–62, §5 p. 19.
- **clutData and dacData want opposite video states.** CLUT writes are ignored while the
  video unit is held in reset [1] §9.5 p. 85; DAC accesses want the memory bus idle — video
  reset plus a NOP flush, or vertical sync plus the flush [1] §5.48 p. 63.
- **Texture memory is write-only** and reads return undefined data; a texture cannot span
  DRAM banks; texbaseaddr may legitimately wrap below zero [1] §8 pp. 80–82.
- **The LFB has a fixed 1024-pixel stride**, off-screen access without clipping is
  undefined, and LFB reads block until the whole pipe drains [1] §7 pp. 78–79.
- **The device cannot interrupt.** Status bit 31 is "not currently implemented... and will
  always return 0x0", the planned pciInterrupt register was deleted from the specification,
  and every driver polls [1] §5.1 p. 24, §13 p. 90, [11].
- **Only one writable bit in the Command register** — memory-access enable. A driver that
  never sets it sees no device [1] §6.3 p. 72; both published drivers set it [5] [6].
- **The class code is zero.** The SST-1 enumerates as an unclassified device, not a display
  controller [1] §6.6 p. 73.
- **The interrupt-line default is a PC IRQ number** (0x5) — meaningless on a Power
  Macintosh, where firmware writes what it likes [1] §6.12 p. 74.
- **The lane-field self-contradiction.** The lfbMode bit table puts the RGBA lanes at bits
  10:9; the LFB prose twice claims "bits(12:9)" [1] §5.20 pp. 47–52, §7.1 p. 78. The bit
  table is authoritative ([6] `sstfb.h` agrees).
- **The LFB Y-origin prose contradicts the bit tables.** §7 describes the LFB Y origin as
  fbzMode "bit(16)" [1] §7 p. 78 — which is depth biasing; the real controls are lfbMode
  bit 13 (and fbzMode bit 17 for rendering) [1] §5.19–5.20 pp. 45–52.
- **First-silicon limits**: alpha-channel blending supports only AZERO and AONE [1]
  §5.18.2 p. 42; fbzMode bit 20 (the pseudo-stencil source compare) "is not implemented in
  FBI revision 1.0" [1] §5.19 p. 46; alphaMode bit 5 (anti-aliasing) "is currently not
  implemented" [1] §5.18 p. 39; the 8-bit palette texture formats require a revision-1 TMU
  [1] §5.50 pp. 64–65.
- **MODE_640 / MODE_800 are referenced but never defined** — §10 points at "the fbiInit0
  register description", which contains no such field [1] §10 p. 86, §5.42 p. 59.
- **The CFM fragment name is shared across generations.** The 1997 library file
  `3DfxGlideLib2.x` (no space) and the 1999–2000 `3dfx GlideLib2.x` (space) register the same
  fragment, with no version tie-break between them (§4.5; [7], [voodoo2.md](voodoo2.md) §5).
- **The Mac stack never touches the interrupt pin even where silicon offers one** — and on
  this generation the silicon offers none at all; the entire 1997 driver set is poll-driven
  (§3.7, §4.5).

## 6. Open questions

1. **The TREX (Texelfx) specification is missing.** trexInit0/trexInit1 get one line each —
   "TO BE COMPLETED. See TREX spec." [1] §5.54–5.55 p. 69 — and the referenced document is
   not in the evidence set. The drivers' default values exist [5] [6] but are untied to any
   register description; the field semantics (mem2, reset_FIFOs, reset_graphics,
   palette_del) are known only from the revision-2.0 change list [1] §12 p. 89.
2. **The MacMagic's board specifics.** The 8 MB configuration and the Mac-specific-ROM claim
   are single-sourced; the Village Tronic driver CD in the evidence set proves the drivers,
   not the board [12]. Whether its ROM is an FCode ROM, and why it would be PC-incompatible,
   is unverified.
3. **The Power3D's board-level details**: which pass-through switch (relay or solid-state)
   [11], the strap values (hence the card's vga_pass power-on state and whether the PCI base
   is hardcoded), the DRAM part count and the exact DB-15 loop wiring. No schematic exists in
   the evidence set.
4. **Which RAMDAC the Power3D carries**, and the video/graphics clocks its installer actually
   programs. The driver probes TI/AT&T/ICS [5] [6]; no Mac-side record of the result exists.
5. **The 1997 Mac library's internals**: its CFM fragment version, whether it reads a
   `.var`/INI configuration file like the second generation's `voodoo2.var`, and its
   Mac-specific init differences. Only the archive's file names and type/creator codes are
   read here; the library binary is not symbol-mapped.
6. **The Mac endianness split per color format**: which of the hardware swizzle paths
   (fbiInit0 bit 3, lfbMode bits 11/12/15/16, tLOD bits 25/26) the 1997 library enables, and
   where software conversion takes over. The second-generation split is itself unrecorded
   ([voodoo2.md](voodoo2.md) §6).
7. **The pciInterrupt design**: what the deleted register's full field layout was, and
   whether any silicon spin ever generated an interrupt. The vRetrace counter it would have
   compared against survives readable [1] §5.37 p. 58.
8. **MODE_640 / MODE_800**: what the §10 reference to the fbiInit0 description was supposed
   to point at [1] §10 p. 86.
9. **The memory FIFO's row arithmetic**: fbiInit4's row start/rollover fields and fbiInit2's
   video buffer offset interact with the tiled layout, but no worked example of row
   placement exists in the specification [1] §5.44–5.46 pp. 61–62.
10. **SLI on this generation**: the specification's own §11 is a stub [1] §11 p. 88; the
    register surface exists but no driver-published SLI bring-up for SST-1 is in the evidence
    set, and no retail Mac card carries a companion connector.
11. **The 4 MB framebuffer banking**: the Linux driver notes 4 MB boards have banked memory
    (fbiInit2 bits 1 and 20) and that its detection fails because "the 2 last Mbs wrap
    around" [6] `sstfb.c` — the banking/wrap semantics are otherwise undocumented.
12. **Which FBI/TREX revisions retail Mac cards carry**, and therefore which revision-2.0
    fixes apply (the texel-drop fix, the proper PCI config decoding, fbzMode bit 20, the
    palette formats) [1] §12 p. 89. The Revision_ID register would answer this on real
    hardware; no Mac-side readout is recorded.
13. **The fog index width**: the specification's "6-bit floating point (1/W)" versus Glide's
    "14-bit floating point representation of 1/w" [1] §3.3 p. 15, [2] Ch. 1 p. 5 — read here
    as 6-bit index plus 8-bit interpolation (*inferred*), but the exact bit split of the
    floating-point representation is not stated by either document.

## References

1. 3Dfx Interactive, Inc., *SST-1 (a.k.a. Voodoo Graphics): High Performance Graphics Engine
   for 3D Game Acceleration*, specification revision 1.61, December 1, 1999 — title page and
   revision history §13 p. 90; §1 "General Description" pp. 6–7 (features, resolution
   table); §2 "Performance" p. 7 (the 50 MHz tables); §3 "Architectural and Functional
   Overview" pp. 8–16 (FBI/TREX division p. 8, TREX-count pass table pp. 8–10, SLI p. 10,
   pixel pipeline p. 12, slave/FIFO/memory architecture p. 13, functional overview
   pp. 13–16); §4 "SST-1 Address Space" p. 17; §5 "Memory Mapped Register Set" pp. 18–71
   (address field decode pp. 18–19, register table pp. 18–23, remapped table pp. 22–23,
   status p. 24, vertex registers p. 25, parameter registers pp. 26–29, triangleCMD p. 30,
   fbzColorPath pp. 31–36, fogMode pp. 37–38, alphaMode pp. 39–42, fbzMode pp. 42–46,
   lfbMode pp. 46–53, clip registers p. 53, commands and constants pp. 54–56, counters
   pp. 57–58, fogTable p. 58, video registers pp. 58–59, fbiInit0–4 pp. 59–62, clutData
   p. 62, dacData p. 63, maxRgbDelta p. 63, textureMode pp. 63–66, tLOD pp. 66–68, tDetail
   p. 68, texBaseAddr p. 68, trexInit0/1 p. 69, nccTable/palette pp. 69–71); §6 "PCI
   Configuration Register Set" pp. 72–77; §7 "Linear Frame Buffer Access" pp. 78–79; §8
   "Texture Memory Access" pp. 80–83; §9 "Programming Caveats" pp. 84–85; §10 "Video Timing"
   pp. 86–87; §11 "Scanline Interleaving" p. 88; §12 "SST-1 Revision 2.0 Changes" p. 89;
   §13 "Revision History" p. 90.
2. 3Dfx Interactive, Inc., *Glide Programming Guide*, version 2.4, July 1997 — Chapter 1
   (Texelfx/Pixelfx naming, TMU configurations, fog indexing, the pixel pipeline,
   pp. 1–6); Chapter 3 "Getting Started" pp. 15–25 (the grSstQueryBoards/grGlideInit/
   grSstQueryHardware/grSstSelect/grSstWinOpen sequence, multiple subsystems and SLI
   pp. 19–20, the display buffer and Table 3.3 pp. 21–23, buffer swapping p. 23, vertical
   retrace pp. 24–25); Chapter 10 "Managing Texture Memory" pp. 95–116 (texture memory
   bounds, 8-byte alignment, even/odd level masks); Chapter 11 "Accessing the Linear Frame
   Buffer" pp. 117–132 (grLfbLock/grLfbWriteRegion and the lock rules).
3. 3Dfx Interactive, Inc., *Glide Reference Manual*, version 2.4, July 1997 — the SST naming
   note (p. 1); `grSstControlMode` p. 67 (GR_CONTROL_ACTIVATE/DEACTIVATE and the
   VGA/Voodoo display switch); `grSstIdle` p. 68; `grSstIsBusy` p. 69; `grBufferNumPending`
   p. 20; `grSstQueryBoards` p. 72; `grSstQueryHardware` p. 73; `grSstVideoLine` p. 79;
   `grSstVRetraceOn` p. 80; `grSstWinOpen` p. 82.
4. Apple Computer, Inc., *Designing PCI Cards and Drivers for Power Macintosh Computers*,
   revised edition (March 26, 1999) — §"No Open Firmware Support" p. 88: enumeration of a
   card with no FCode or no expansion ROM (recognized, address space allocated, no driver
   loaded, no name property).
5. 3Dfx Interactive, Inc., Glide run-time source code release (Glide 2.x, open source) — the
   SST-1 initialization module `glide2x/sst1/init/initvg/`: `sst1init.c` (PCI device search
   with vendorID 0x121A / deviceID 1, the `SST_*` environment-variable table,
   SST_GRXCLK "16 <= Frequency < 80 (50 is default)"), `sst1init.h`
   (SST_FBIINIT0–4_DEFAULT 0x00000410 / 0x00201102 / 0x80000040 / 0x001E4000 / 0x00000001,
   SST_TREXINIT0_DEFAULT field set), `dac.c` (the TI/AT&T/ICS detection probes).
6. Linux kernel `sstfb` framebuffer driver (`drivers/video/sstfb.c`, `sstfb.h`,
   `Documentation/fb/sstfb.rst`) and `include/linux/pci_ids.h` — the independent second
   driver: `sst_init`/`sst_shutdown`/`sstfb_set_par` and the Voodoo-1-specific tile and
   validity arithmetic, `sst_calc_pll` (Fout = Fref × (M+2) / (2^P × (N+2))), the DAC
   register constants (MIR/DIR 0x84/0x97 and 0x09, ICS PLL power-on values 0x55/0x71/0x79),
   the `__BIG_ENDIAN` swizzle branch and its comment, the `vgapass`/`clipping`/`gfxclk`
   options, and the PCI ID table (0x121A/0x0001 Voodoo Graphics, 50 MHz default clock
   capped at 60).
7. 3dfx Interactive / TechWorks, *Voodoo Graphics driver for Macintosh*, release of
   1997-08-27 (archive "VOODOO_DRV_08-27-1997.zip" wrapping the StuffIt self-extractor
   "VOODOO_DRV_08-27-1997.sea"), opened and read as raw strings: the folder "Power3D MacOS",
   the "Drag files onto System Folder" install instruction, and the two files "3Dfx RAVE"
   (`shlb`/`tnsl`, with a `shlb`/`QD3D` QuickDraw 3D Accelerator reference) and
   "3DfxGlideLib2.x" (`shlb`/`????`); no `'ndrv'`, extension or OpenGL renderer appears.
8. 3dfxBIOS, "VoodooMAC" Macintosh driver index (saved reference page) — the Voodoo Graphics
   Macintosh driver release list: "Drivers only", version unknown, dated 08/27/1997; the
   Voodoo2 betas of 1999–2000 for comparison.
9. Macintosh Garden, "Techworks Power3D 3dfx Drivers" item page (saved reference page; the
   TechWorks driver CD content, publication year 1997) — TechWorks's own Power3D package
   copy: feature list ("full-screen 3D acceleration", "native 3Dfx Glide API support",
   640x480, RAVE), performance claims (45 Mpixels/s sustained fill, "over 1 million
   triangles per second" on 25-pixel fully featured triangles), and system requirements
   (MacOS 7.5.3 or greater, any PowerPC with a free PCI slot except all-in-one systems,
   32 MB RAM).
10. Internet Archive, "Techworks Power3D 3dfx Voodoo Card for Power PC Mac Installation
    Disks" item page (saved reference page) — the installation disk images for the
    TechWorks Power3D, publication date 1997, item photographs DSC_3435–3438.
11. Wikipedia, "3dfx" article, §"Voodoo Graphics PCI" (saved reference page) — the typical
    card configuration (DAC + frame buffer processor + one TMU + 4 MB EDO DRAM at 50 MHz),
    the pass-through VGA cable and the relay versus solid-state switch (with the audible
    relay click), and the polling driver consequence of the missing interrupt mechanism.
12. Macintosh Garden, "Village Tronic Graphics Drivers" item page (saved reference page) —
    the Village Tronic driver CD, including drivers for "MacPicasso: 320/328/340/516/520/523/
    540/750/850" and "MacMagic".
