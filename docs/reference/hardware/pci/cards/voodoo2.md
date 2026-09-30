# 3dfx Voodoo2 (CVG)

The **3dfx Voodoo2** — 3dfx's internal names *CVG*, "Console Voodoo Graphics", and *SST2* — is a
second-generation 3D-only graphics accelerator chipset: a set of three chips (one frame-buffer
interface, two texture-mapping units) that renders full-screen 3D into its own private memory
and passes the host's 2D video signal through to the monitor when no 3D context is active. It
is not a display card in the ordinary sense: it has no 2D engine the host addresses as a
framebuffer, it carries no expansion ROM of any kind, and it cannot be the boot display of any
machine it plugs into. On the PCI Power Macintosh it is a consumer add-on, sold as the
**TechWorks Power3D II** and the **Micro Conversions Game Wizard**, that turns QuickDraw 3D
RAVE, Glide and OpenGL titles into accelerated full-screen 3D while the machine's built-in
video keeps the desktop.

This page documents the hardware: the PCI configuration space, the 16 MB memory-mapped
aperture and its register file, the pixel pipeline and texture engine, the video backend and
its external RAMDAC, and the software contract — the 3dfx register specification [1], the
open-sourced Glide bring-up code [6], a second independent operating-system driver [7], and
the shipped Macintosh driver set, whose archives open directly and whose Read Me texts,
import tables and symbol strings are quoted here [8]. The host side of the bus —
configuration cycles, BAR assignment, the memory windows — is the PCI page's subject
([pci.md](../pci.md)); the bridge that implements it on the machines these cards shipped
into is [bandit.md](../../../machines/tnt/bandit.md), the family map is
[tnt.md](../../../machines/tnt/tnt.md) §3.

**Contents:**

1. [Overview](#1-overview) — what the part is, the Chuck/Bruce architecture, the Mac cards,
   the ROM-less pass-through identity, clocking and memory configurations
2. [Register file](#2-register-file) — the 16 MB aperture, the register address decode, the
   complete register map, per-register detail: status and interrupts, the rendering state,
   triangle parameters and setup, fbiInit0–7, video and DAC, the TMU file, the 2D engine,
   CMDFIFO, PCI configuration space
3. [Behaviour](#3-behaviour) — the PCI slave and its FIFOs, the pixel pipeline, triangle
   rendering, linear framebuffer access, texture memory, video timing, interrupts, passthrough
   and snooping, buffer swaps and SLI, the 2D BitBLT engine
4. [Programming model](#4-programming-model) — power-on bring-up, DAC detection and clocks,
   a real mode-set, the Glide contract, the Mac driver stack, Mac-side discovery,
   endianness, the configuration variables
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 What the part is

"Voodoo2 Graphics from 3dfx Interactive is a second generation 3D graphics accelerator
specifically designed to address the requirements of the game console, location-based
entertainment, arcade, and PC game enthusiast markets" [1] §1 p. 7. It was introduced in
February 1998 [12], replacing the two-chip Voodoo Graphics (SST-1) [2], whose register
architecture it inherits and extends: the Voodoo2 specification is organized as a delta on
SST-1, the revision history shows the document was born as "Console Voodoo Graphics" and
renamed at revision 1.12 [1] §14 p. 132, and several sections (the TRIANGLE command, the
Bruce initialization registers) defer outright to the SST-1 and Bruce documents [1] §7.2
p. 108, §5.90 p. 85.

The defining properties, all from the specification's own summary [1] §1 p. 7–8:

- It is **3D-only**: no 2D core, no VGA controller, no BIOS. The host's existing 2D card
  feeds its monitor output through the Voodoo2 board, and the Voodoo2's own video backend
  takes over only while a 3D context is active (§3.8).
- It is a **PCI slave only** [1] §3.3 p. 15: the device never masters the bus, there is no
  DMA engine in either direction, and every command, texture and pixel the chip consumes is
  pushed to it by the host CPU. The PCI configuration space accordingly hard-wires the
  bus-mastering registers to zero [1] §6.14–6.15 p. 105.
- It occupies **16 MB of memory-mapped PCI address space and no I/O space** — "Voodoo2
  Graphics does not utilize I/O mapped memory", and "All I/O accesses to Voodoo2 Graphics are
  ignored" [1] §4 p. 20, §12.1 p. 128.
- It is **PCI 2.1 compliant at up to 66 MHz** [1] §1 p. 8, with the slave interface
  supporting zero-wait-state and burst transfers [1] §3.3 p. 15.
- It is **bi-endian**: byte-swizzle support for register accesses, linear framebuffer
  accesses and texture downloads exists in hardware precisely because the designers expected
  big-endian hosts — "big endian CPUs (e.g. PowerPC processors) should enable byte swizzling"
  [1] §1 p. 8, §5.21 p. 56. This is the single most Mac-relevant silicon feature (§4.7).

### 1.2 Chuck and Bruce

In its entry-level configuration "a Voodoo2 Graphics graphics solution consists of two
rendering ASICs: Chuck and Bruce" [1] §3.1 p. 11:

- **Chuck** (the frame-buffer interface, FBI) is the PCI slave — "all communication from the
  host CPU to Voodoo2 Graphics is performed through Chuck". It implements triangle setup,
  Gouraud shading, alpha blending, fogging, depth-buffering and dithering, the 2D BitBLT
  engine, all linear framebuffer access, and the video display controller that drives the
  monitor [1] §3.1 p. 11.
- **Bruce** (the texture-mapping unit, TMU; the register file calls it Trex) "implements all
  of the texture mapping capabilities": the per-pixel perspective divide, LOD mipmapping,
  bilinear filtering, and the advanced modes (detail, projected, trilinear) [1] §3.1 p. 11.
  Bruce has its own dedicated 64-bit memory controller for texture memory [1] §3.3 p. 15.

A retail card is one Chuck plus one or two Bruces. With a single Bruce, the advanced modes
are two-pass; with two, trilinear/detail/projected become single-pass; three Bruces allow
single-pass everything [1] §3.1 pp. 11–13. Multiple Chuck/Bruce subsystems can also be chained
by scan-line interleaving (SLI), doubling the rendering rate [1] §3.1 p. 13 (§3.9). The
consumer cards the Mac market saw are single-Chuck, two-Bruce designs: the specification's
own entry-level drawing, and the standard 8 MB (2 MB per TMU) and 12 MB (4 MB per TMU)
boards [12]; the TechWorks Power3D II is documented as a 12 MB, 90 MHz SSTV2 board with one
FBI and two TMUs [10].

The 3D feature list [1] §1 pp. 7–8 names the headline capabilities — hardware triangle
setup with backface culling, strips and fans, sub-pixel correction to 0.4 × 0.4,
perspective-corrected texture mapping with per-pixel divide, trilinear mipmapping with
detail and projected textures, the compressed and paletted texture formats with a
512-entry palette, W-buffering, eight depth and alpha comparison functions,
chroma-keying, per-pixel fog through a 64-entry table, and ordered dithering to the
16-bit framebuffer — each of which maps onto the register file of §2.5 and the pipeline
of §3.2. Peak performance is stated at a 75 MHz graphics clock: one pixel per clock for
triangles, two for FASTFILL clears, sixteen for SGRAM color-expansion clears [1] §2
p. 9, §3.3 p. 15.

### 1.3 The Mac cards

3dfx never built a Mac-specific Voodoo2 board. The two products sold into the PCI Power
Macintosh market are rebranded reference designs:

| Card | Silicon | Memory | Evidence |
|---|---|---|---|
| TechWorks Power3D II | SSTV2, 350 nm, 4 M transistors, 90 MHz clock | 12 MB EDO (4 MB framebuffer + 2 × 4 MB texture), 192-bit total memory bus | [10] |
| Micro Conversions Game Wizard | Voodoo2 | 8 MB and 12 MB versions; the Mac version uses a DB-15 video output | [11] |

The Micro Conversions card is documented in the same breath as its PC siblings ("Micro
Conversions Game Wizard is version for Apple computers using DB15 video output" [11]) —
the Mac variant differs in the pass-through connector, not in silicon or firmware
(*inferred* from [11] plus the card-agnostic driver requirement below; no Mac card
schematic or bill of materials exists in the evidence set). That reading is confirmed by
the driver, which is deliberately card-agnostic: the system
requirements ask only for "Any PCI based 3dfx Voodoo2 based 3D graphics accelerator (iMac
cards are not currently supported)" [8]. Nothing in the Mac software stack distinguishes a
Power3D II from a Game Wizard (*inferred*); both answer the same PCI vendor/device ID
(§2.12) and the same register file. Mac support was shipped by 3dfx itself as a series
of driver betas
(1.0 beta 2 of 1999-08-10, beta 4 of 1999-12-06, beta 5 of 2000-01-25 [9]) and never left
beta; the beta 5 Read Me states plainly that the drivers "are delivered 'as-is' and are not
offered with any support" [8].

The pass-through scheme on a Power Macintosh is the same as on a PC with one difference
that matters to cabling: the host 2D output is the built-in video of the Power Mac, whose
monitor connector is DB-15, so the Mac cards carry a DB-15 loop-through [11] where PC cards
carry HD-15.

### 1.4 The ROM-less identity

The Voodoo2 carries **no expansion ROM**: no VGA BIOS, no Open Firmware FCode, no
name-producing device firmware. A Macintosh booting Open Firmware therefore does what
Apple's PCI book specifies for "an expansion ROM with no FCode, or no expansion ROM at
all": "the card is recognized and address space is allocated for the device, but no
peripheral initialization or driver code is loaded... there is no distinct name property
for the device" [5] §"No Open Firmware Support" p. 88. Concretely, on a TNT-family machine
the card is enumerated on the Bandit bus, its single 16 MB memory BAR is probed and
assigned a PCI memory address ([bandit.md](../../../machines/tnt/bandit.md) §4.2; the window
it lands in is [tnt.md](../../../machines/tnt/tnt.md) §3.3), and the Open Firmware device
tree records the node with its `reg`/`assigned-addresses` properties but no display-driver
FCode — the card is invisible as a boot device and the built-in video remains the startup
screen. The device cannot be the machine's primary display under any circumstances: it has
no 2D mode, and its video backend only outputs while a Glide/RAVE/OpenGL context holds it
active (§3.8).

All device identity therefore flows through PCI configuration space: vendor ID 0x121A
(3Dfx Interactive), device ID 0x0002 (Voodoo2) — the value the shipping hardware reports,
per the two independent drivers that match on it [6] `sst1Init.c` (deviceID 0x0002) and
[7] `pci_ids.h` (`PCI_VENDOR_ID_3DFX 0x121a`, `PCI_DEVICE_ID_3DFX_VOODOO2 0x0002`). The
3dfx specification itself prints a different story — "Device Identification. Default is
0x1" [1] §6.2 p. 102 — which is word-for-word the SST-1 document's device-ID text [2] §6.2
p. 102: the section was copied from the Voodoo Graphics specification and the default value
never updated. The hardware, the Windows driver source and the Linux kernel agree on 0x0002;
the 0x0001 ID belongs to the SST-1 device [7]. (See §5.)

### 1.5 Clocking and memory configurations

The specification states memory-system facts at a 75 MHz graphics clock and supports "2 or
4 MBytes of SGRAM or SDRAM frame buffer memory" and "2, 4, 8, or 16 MBytes of SGRAM or
SDRAM texture memory" [1] §1 p. 8. Chuck's framebuffer controller is a 64-bit interleaved
datapath to RGB and alpha/depth memory; Bruce's texture controller is a separate 64-bit
datapath, with interleaving and texture caching arranged so "bilinear texture filtering
with no performance penalty relative to point sampling" [1] §3.3 p. 15. The framebuffer
budget table: 2 MB supports 800×600×16 double-buffered without depth, 640×480×16 triple
buffered, or 640×480×16 double-buffered with a 16-bit depth buffer; 4 MB raises the
depth-buffered mode to 800×600×16 [1] §1 p. 8.

Retail silicon runs faster than the specification's assumptions: the shipping clock is
90 MHz with 100 MHz EDO DRAM [12], [10]. The graphics clock is not fixed by any strap —
it is programmed at driver bring-up through the external DAC's PLL (§4.2), which is why
the same chipset shipped at a spread of clocks and why the driver carries an override knob
for it ([6] `sst1InitCalcGrxClk`, `SSTV2_GRXCLK`; §4.8).

Framebuffer memory organization is tiled, in 32×32-pixel tiles: the revision history
records the change from SST-1's 64×16 tiling ("Changed tiling algorithm from 64x16 tiles
to 32x32 tiles", revision 1.11) [1] §14 p. 132, and the number of tiles in X is a
six-bit value assembled from three fbiInit bits — {fbiInit1[24], fbiInit1[7:4],
fbiInit6[30]} [1] §5.53 p. 68, §5.58 p. 72. The Y dimension of the video window is what
the LFB and rendering engines see as 1024 scanlines regardless of memory size [3]
Ch. 3 §"Logical Layout of the Linear Frame Buffer".

## 2. Register file

### 2.1 The 16 MB aperture

Everything the host can touch lives in one 16 MB memory-mapped window, assigned by the
memBaseAddr configuration register and compared against PCI address bits 31:24 for decode
[1] §6.11 p. 104. The internal layout [1] §4 p. 20:

| Aperture offset | Size | Contents |
|---|---|---|
| 0x000000–0x3FFFFF | 4 MB | memory-mapped register set |
| 0x400000–0x7FFFFF | 4 MB | linear frame buffer access |
| 0x800000–0xFFFFFF | 8 MB | texture memory access (write only; reads return undefined data [1] §10 p. 116) |

When the CMDFIFO transport is enabled (fbiInit7 bit 8 = 1), the register map shrinks to
2 MB and the next 2 MB becomes the write-only CMDFIFO port [1] §11.2 p. 121:

| Aperture offset | Size | Contents (CMDFIFO map) |
|---|---|---|
| 0x000000–0x1FFFFF | 2 MB | memory-mapped register set (restricted, §3.1) |
| 0x200000–0x3FFFFF | 2 MB | CMDFIFO (write only; reads undefined) |
| 0x400000–0x7FFFFF | 4 MB | linear frame buffer access (unchanged) |
| 0x800000–0xFFFFFF | 8 MB | texture memory access (unchanged) |

The legacy (register-mapped) map is selected when fbiInit7 bit 8 = 0 [1] §11.1 p. 120.

### 2.2 The register address decode

A legacy-map register address is not a flat offset: bits 21:0 of the aperture offset are
decoded as fields [1] §5 pp. 21–22:

| Bits | Field | Meaning |
|---|---|---|
| 21 | alternate register mapping | when fbiInit3(0)=1 **and** bit 21 is set, the alternate (remapped) triangle-parameter layout of §2.6 applies [1] §5 p. 21, §5.55 p. 70 |
| 20 | byte-swizzle register accesses | when fbiInit0(3)=1 **and** bit 20 is set, bytes 3:0 of the data are swapped pairwise (3↔0, 2↔1) on both reads and writes — the big-endian path (§4.7) [1] §5 p. 21 |
| 19:14 | wrap | aliases the 14-bit register map; exists so processors with collapsing write buffers (the spec names the Alpha and Pentium Pro) can issue back-to-back writes to the same register without merging. Functionality is identical for every wrap value [1] §5 p. 21 |
| 13:10 | chip select | a bitmap: bit 0 = Chuck, bit 1 = Bruce #0, bit 2 = Bruce #1, bit 3 = Bruce #2; 0000 selects all chips. **Reads ignore this field and always return data from Chuck** [1] §5 p. 22 |
| 9:2 | register index | selects the register [1] §5 p. 21 |
| 1:0 | byte | must be 0 |

Three access rules govern the whole file [1] §5 p. 22, §12.2 p. 128: all register accesses
must be 32-bit (byte and halfword accesses are illegal); there are no bit-set/bit-clear
aliases, so modifying one bit means writing the whole longword; and reading a write-only
register returns undefined data (as does reading any Bruce-resident register, since reads
come from Chuck). The same paragraph defines the pipelining/FIFO split that §3.1 relies
on: a "no" in the pipelined column means the engine flushes its pipe before loading that
register, and a "no" in the FIFO column means the write bypasses the PCI FIFO and takes
effect immediately.

### 2.3 Register map (legacy, fbiInit3(0)=0)

The complete map, at aperture offset 0x000000 + chip-select wrap; the Chip column shows
which silicon latches the register on writes (C = Chuck only, C+B = Chuck and the
selected Bruces, B = the selected Bruces, B1 = Bruce #1 only — TMU1's private parameters).

| Offset | Register | Bits | Chip | R/W | Pipe/FIFO |
|---|---|---|---|---|---|
| 0x000 | status | 31:0 | C | R | yes/n-a |
| 0x004 | intrCtrl | 31:0 | C | R/W | yes/no |
| 0x008–0x01C | vertexAx/Ay/Bx/By/Cx/Cy | 15:0 | C+B | W | yes/yes |
| 0x020–0x03C | startR, startG, startB, startZ, startA, startS, startT, startW | 23:0/31:0 | C, C, C, C, C, B, B, C+B | W | yes/yes |
| 0x040–0x07C | dRdX…dWdY (X: R,G,B,Z,A,S,T,W; then Y: same) | 23:0/31:0 | C or B per component | W | yes/yes |
| 0x080 | triangleCMD | 31 | C+B | W | yes/yes |
| 0x084 | reserved | — | — | — | — |
| 0x088–0x0FC | fvertex*, fstart*, fd*d* (float mirrors of the above) | 31:0 | as above | W | yes/yes |
| 0x100 | ftriangleCMD | 31 | C+B | W | yes/yes |
| 0x104 | fbzColorPath | 29:0 | C+B | R/W | yes/yes |
| 0x108 | fogMode | 7:0 | C | R/W | yes/yes |
| 0x10C | alphaMode | 31:0 | C | R/W | yes/yes |
| 0x110 | fbzMode | 21:0 | C | R/W | no/yes |
| 0x114 | lfbMode | 16:0 | C | R/W | no/yes |
| 0x118 | clipLeftRight | 31:0 | C | R/W | no/yes |
| 0x11C | clipLowYHighY | 31:0 | C | R/W | no/yes |
| 0x120 | nopCMD | 1:0 | C+B | W | no/yes |
| 0x124 | fastfillCMD | n/a | C | W | no/yes |
| 0x128 | swapbufferCMD | 9:0 | C | W | no/yes |
| 0x12C | fogColor | 23:0 | C | W | no/yes |
| 0x130 | zaColor | 31:0 | C | W | no/yes |
| 0x134 | chromaKey | 23:0 | C+B | W | no/yes |
| 0x138 | chromaRange | 27:0 | C+B | W | no/yes |
| 0x13C | userIntrCMD | 9:0 | C | W | no/yes |
| 0x140 | stipple | 31:0 | C | R/W | no/yes |
| 0x144 | color0 | 31:0 | C | R/W | no/yes |
| 0x148 | color1 | 31:0 | C | R/W | no/yes |
| 0x14C | fbiPixelsIn | 23:0 | C | R | n-a |
| 0x150 | fbiChromaFail | 23:0 | C | R | n-a |
| 0x154 | fbiZfuncFail | 23:0 | C | R | n-a |
| 0x158 | fbiAfuncFail | 23:0 | C | R | n-a |
| 0x15C | fbiPixelsOut | 23:0 | C | R | n-a |
| 0x160–0x1DC | fogTable[0..31] (64 entries, two per word) | 31:0 | C | W | no/yes |
| 0x1E0 | cmdFifoBaseAddr | 25:0 | C | R/W | n-a/no |
| 0x1E4 | cmdFifoBump | 15:0 | C | R/W | n-a/no |
| 0x1E8 | cmdFifoRdPtr | 31:0 | C | R/W | n-a/no |
| 0x1EC | cmdFifoAMin | 31:0 | C | R/W | n-a/no |
| 0x1F0 | cmdFifoAMax | 31:0 | C | R/W | n-a/no |
| 0x1F4 | cmdFifoDepth | 15:0 | C | R/W | n-a/no |
| 0x1F8 | cmdFifoHoles | 15:0 | C | R/W | n-a/no |
| 0x1FC | reserved | — | — | — | — |
| 0x200 | fbiInit4 | 12:0 | C | R/W | n-a/no |
| 0x204 | vRetrace | 12:0 | C | R | n-a/no |
| 0x208 | backPorch | 24:0 | C | R/W | n-a/no |
| 0x20C | videoDimensions | 26:0 | C | R/W | n-a/no |
| 0x210 | fbiInit0 | 31:0 | C | R/W | n-a/no |
| 0x214 | fbiInit1 | 31:0 | C | R/W | n-a/no |
| 0x218 | fbiInit2 | 31:0 | C | R/W | n-a/no |
| 0x21C | fbiInit3 | 31:0 | C | R/W | n-a/no |
| 0x220 | hSync | 26:0 | C | W | n-a/no |
| 0x224 | vSync | 28:0 | C | W | n-a/no |
| 0x228 | clutData | 29:0 | C | W | no/yes |
| 0x22C | dacData | 13:0 | C | W | n-a/no |
| 0x230 | maxRgbDelta | 23:0 | C | W | n-a/no |
| 0x234 | hBorder | 24:0 | C | W | n-a/no |
| 0x238 | vBorder | 24:0 | C | W | n-a/no |
| 0x23C | borderColor | 23:0 | C | W | n-a/no |
| 0x240 | hvRetrace | 26:0 | C | R | n-a/no |
| 0x244 | fbiInit5 | 31:0 | C | R/W | n-a/no |
| 0x248 | fbiInit6 | 31:0 | C | R/W | n-a/no |
| 0x24C | fbiInit7 | 31:0 | C | R/W | n-a/no |
| 0x250, 0x254 | reserved | — | — | — | — |
| 0x258 | fbiSwapHistory | 31:0 | C | R | n-a |
| 0x25C | fbiTrianglesOut | 23:0 | C | R | n-a |
| 0x260 | sSetupMode | 19:0 | C | W | yes/yes |
| 0x264 | sVx | 31:0 | C+B | W | yes/yes |
| 0x268 | sVy | 31:0 | C+B | W | yes/yes |
| 0x26C | sARGB | 31:0 | C+B | W | yes/yes |
| 0x270 | sRed | 31:0 | C | W | yes/yes |
| 0x274 | sGreen | 31:0 | C | W | yes/yes |
| 0x278 | sBlue | 31:0 | C | W | yes/yes |
| 0x27C | sAlpha | 31:0 | C | W | yes/yes |
| 0x280 | sVz | 31:0 | C | W | yes/yes |
| 0x284 | sWb | 31:0 | C+B | W | yes/yes |
| 0x288 | sWtmu0 | 31:0 | B | W | yes/yes |
| 0x28C | sS/W0 | 31:0 | B | W | yes/yes |
| 0x290 | sT/W0 | 31:0 | B | W | yes/yes |
| 0x294 | sWtmu1 | 31:0 | B1 | W | yes/yes |
| 0x298 | sS/Wtmu1 | 31:0 | B1 | W | yes/yes |
| 0x29C | sT/Wtmu1 | 31:0 | B1 | W | yes/yes |
| 0x2A0 | sDrawTriCMD | 31:0 | C+B | W | yes/yes |
| 0x2A4 | sBeginTriCMD | 31:0 | C | W | yes/yes |
| 0x2A8–0x2BC | reserved | — | — | — | — |
| 0x2C0 | bltSrcBaseAddr | 21:0 | C | R/W | yes/yes |
| 0x2C4 | bltDstBaseAddr | 21:0 | C | R/W | yes/yes |
| 0x2C8 | bltXYStrides | 27:0 | C | R/W | yes/yes |
| 0x2CC | bltSrcChromaRange | 31:0 | C | R/W | yes/yes |
| 0x2D0 | bltDstChromaRange | 31:0 | C | R/W | yes/yes |
| 0x2D4 | bltClipX | 27:0 | C | R/W | yes/yes |
| 0x2D8 | bltClipY | 27:0 | C | R/W | yes/yes |
| 0x2DC | reserved | — | — | — | — |
| 0x2E0 | bltSrcXY | 26:0 | C | R/W | yes/yes |
| 0x2E4 | bltDstXY | 31:0 | C | R/W | yes/yes |
| 0x2E8 | bltSize | 31:0 | C | R/W | yes/yes |
| 0x2EC | bltRop | 15:0 | C | R/W | yes/yes |
| 0x2F0 | bltColor | 31:0 | C | R/W | yes/yes |
| 0x2F4 | reserved | — | — | — | — |
| 0x2F8 | bltCommand | 31:0 | C | R/W | yes/yes |
| 0x2FC | bltData | 31:0 | C | W | yes/yes |
| 0x300 | textureMode | 30:0 | B | W | yes/yes |
| 0x304 | tLOD | 27:0 | B | W | yes/yes |
| 0x308 | tDetail | 21:0 | B | W | yes/yes |
| 0x30C | texBaseAddr | 18:0 | B | W | yes/yes |
| 0x310 | texBaseAddr_1 | 18:0 | B | W | yes/yes |
| 0x314 | texBaseAddr_2 | 18:0 | B | W | yes/yes |
| 0x318 | texBaseAddr_3_8 | 18:0 | B | W | yes/yes |
| 0x31C | trexInit0 | 31:0 | B | W | no/yes |
| 0x320 | trexInit1 | 31:0 | B | W | no/yes |
| 0x324–0x350 | nccTable0[0..11] | 31:0/26:0 | B | W | no/yes |
| 0x354–0x380 | nccTable1[0..11] | 31:0/26:0 | B | W | no/yes |
| 0x384–0x3FC | reserved | — | — | — | — |

[1] §5 pp. 22–26. The TMU registers (0x300–0x380) are dispatched to the Bruce or Bruces
selected by the chip field; a write with chip-select 0001 (Chuck only) reaches no TMU, and
TMU1-only registers (sWtmu1 and friends) only exist when a second TMU is present.

### 2.4 status and intrCtrl

**status** (0x000, read-only; writing has no effect) [1] §5.1 p. 29:

| Bits | Meaning |
|---|---|
| 5:0 | PCI FIFO free space, in entries (0x3F = empty); the host FIFO is 64 entries deep |
| 6 | vertical retrace (0 = retrace active, 1 = inactive) |
| 7 | Chuck graphics engine busy |
| 8 | Bruce busy — set "if any unit in Bruce is not idle... includes the graphics engine and all internal Bruce FIFOs" |
| 9 | Voodoo2 busy — set when any internal unit (engines, FIFOs) is active |
| 11:10 | displayed buffer (0 = buffer 0, 1 = buffer 1, 2 = auxiliary buffer, 3 = reserved) |
| 27:12 | memory FIFO free space (0xFFFF = empty); up to 65536 entries when enabled |
| 30:28 | swap buffers pending — incremented on each SWAPBUFFER entering the front-end FIFO, decremented on each completed swap |
| 31 | reserved |

**intrCtrl** (0x004) [1] §5.2 p. 30: bits 5:0 are interrupt-enable masks (bit 0 hsync
rising, 1 hsync falling, 2 vsync rising, 3 vsync falling, 4 PCI-FIFO-full, 5 user
interrupt); bits 11:6 are the generated flags; bits 19:12 hold the tag of the most recent
USERINTERRUPT; bit 31 mirrors the external `pci_inta` pin, active low. An interrupt is
cleared by writing 0 to its flag bit **and** 1 to bit 31. The register is not FIFOed —
"writes to intrCtrl are processed immediately" and may therefore pass queued graphics
writes — and no interrupt reaches the PCI pin unless configuration initEnable bit 20 is
set. The FIFO-full interrupt fires when the frontend free-entry count drops below the
threshold in fbiInit0 bits 10:6.

### 2.5 The rendering state

**fbzColorPath** (0x104) controls the color/alpha combine units — the source selection for
RGB (bits 1:0: iterated RGB, Bruce color output, color1 RGB, reserved) and alpha (bits
3:2), the local-select muxes, and the per-unit blend arithmetic (cc_/cca_ zero_other,
sub_clocal, mselect, reverse_blend, add_clocal, add_alocal, invert_output, bits 4–25), plus
three global controls: bit 26 sub-pixel-correction adjust, bit 27 texture-mapping enable,
bit 28 RGBA/Z/W parameter clamping, bit 29 anti-aliasing enable [1] §5.17 pp. 36–40.

**fogMode** (0x108) enables fog and selects its equation: bit 0 enable; bits 2:1 the
fogadd/fogmult muxes giving the four forms `Cout = Afog*Cfog + (1-Afog)*Cin` etc.; bits
4:3 select an alternative fog-alpha source (iterated alpha, high Z bits, low W bits); bit
5 constant-fogColor add; bit 6 dither of the blend factor; bit 7 signed fog-table values
(fog "zones") [1] §5.18 pp. 41–43.

**alphaMode** (0x10C): bit 0 alpha-test enable; bits 3:1 the comparison function (0=never …
7=always, 8 functions); bit 4 alpha-blend enable; bits 15:12/11:8 the destination/source RGB
blend factors (AZERO, ASRC_ALPHA, A_COLOR, ADST_ALPHA, AONE, AOMSRC_ALPHA, AOM_COLOR,
AOMDST_ALPHA, and 0xF = ASATURATE for source / color-before-fog for destination); bits
23:20/19:16 the same for the alpha channel (AZERO or AONE only); bits 31:24 the reference
value [1] §5.19 pp. 43–46.

**fbzMode** (0x110) [1] §5.20 pp. 46–50: bit 0 clipping-rectangle enable; bit 1
chroma-key/range enable; bits 2 and 12 stipple masking (rotate mode / pattern mode, with
the rotate/pattern algorithms spelled out); bit 3 floating-point depth select; bit 4
depth-buffering enable; bits 7:5 depth function; bit 8 dither enable; bit 11 dither
algorithm (0 = 4×4, 1 = 2×2); bits 9/10 RGB / depth write masks; bit 13 alpha-channel
mask; bits 15:14 draw buffer (front/back); bit 16 depth biasing (from zaColor);
bit 17 rendering Y origin (top/bottom); bit 18 alpha planes enable; bit 19
alpha-blending dither subtraction; bit 20 depth source-compare select (zaColor[15:0]);
bit 21 depth float select (W or Z iterator).

**lfbMode** (0x114) [1] §5.21 pp. 50–57: bits 3:0 the write format (0 = 16-bit RGB 5-6-5,
1 = x-5-5-5, 2 = ARGB 1-5-5-5, 4 = 24-bit x-8-8-8, 5 = 32-bit ARGB 8-8-8-8, 12–15 the
depth+color packed formats, 15 depth-only); bits 5:4 write buffer select (front/back);
bits 7:6 read buffer select (front, back, depth/alpha); bit 8 route LFB writes through
the pixel pipeline; bits 10:9 the RGBA lane order (ARGB, ABGR, RGBA, BGRA); bit 11
16-bit word swap (pixel packing and Z/RGB packing for formats 12–15); bit 12 byte swizzle
of LFB writes (§4.7); bit 13 LFB Y origin; bit 14 W source for pipelined LFB writes;
bit 15 16-bit word swap of LFB reads.

The constants zaColor (0x130: bits 15:0 depth, 31:24 constant alpha), fogColor (0x12C),
chromaKey (0x134), chromaRange (0x138: upper limits, per-component inclusive/exclusive
modes bits 24–26, intersection/union bit 27, enable bit 28), color0/color1 (0x144/0x148,
the combine units' local constants and the FASTFILL clear color), stipple (0x140) and the
64-entry fogTable (0x160–0x1DC, two entries per word, blending factor 8.0 plus delta 6.2,
32 writes for the whole table) complete the rendering state [1] §5.25–5.33, §5.41 pp.
59–64.

### 2.6 Triangle parameters and the setup engine

Two complete parameter sets feed the rasterizer. The **integer set** — vertexAx/Ay/Bx/By/
Cx/Cy at 0x008–0x01C in 12.4 fixed point, startR/G/B (12.12), startZ (20.12), startA
(12.12), startS/startT (14.18), startW (2.30), the matching dRdX/dRdY families — is joined
by an **IEEE-single mirror** at 0x088–0x0FC (fvertex*, fstart*, fd*d*); the chip converts
either form to its internal fixed point [1] §5.3–5.15 pp. 31–36. Writing triangleCMD
(0x080) or ftriangleCMD (0x100) launches the triangle; the value written is the triangle's
signed area — bit 31 = 1 means clockwise, bits 30:0 are ignored [1] §5.16 p. 36.

The S/T/W parameters carry a subtlety inherited from SST-1: the hardware iterates
**S/W, T/W and 1/W** — "the W value used by Voodoo2 Graphics for rendering is actually the
reciprocal of the 3D-geometry-calculated W value" — and divides the iterated S and T by
the iterated W per pixel for perspective correction [1] §5.6–5.7 pp. 32–33.

The Voodoo2's headline addition over SST-1 is the **on-chip triangle setup engine**: a
strip/fan parameter block (all IEEE single) at 0x260–0x2A4. sSetupMode (0x260) selects
which parameters the setup unit computes (bits 0–7: RGB, A, Z, Wb, W0, S0/T0, W1, S1/T1),
the primitive type (bit 16: strip/fan), culling (bits 17–18: enable and sign) and the
ping-pong sign inversion of strips (bit 19) [1] §5.69 p. 76. Vertices are streamed through
sVx/sVy/sARGB (or the separate sRed/sGreen/sBlue/sAlpha), sVz, sWb, the TMU0 and TMU1
W/S/T groups; a write to sBeginTriCMD (0x2A4) starts a new strip without drawing, and each
write to sDrawTriCMD (0x2A0) closes a triangle from the last three vertices [1]
§5.70–5.85 pp. 76–79. With culling enabled the setup unit discards back-facing
triangles before they ever reach the walker — such triangles do not increment
fbiTrianglesOut [1] §5.34 p. 62.

When fbiInit3(0)=1, the chip-select wrap bit 21 selects the **alternate mapping**, which
re-lays the triangle-parameter registers so that each parameter's start/dX/dY are
adjacent — "to improve PCI bus throughput" [1] §5 pp. 26–29 (the full alternate table is
printed there; every non-triangle register keeps its address).

### 2.7 fbiInit0–fbiInit7

Eight Chuck initialization registers (0x210, 0x214, 0x218, 0x21C, 0x200, 0x244, 0x248,
0x24C) configure the silicon. They share one contract [1] §5.52–5.59 pp. 67–73: writes
are **ignored unless PCI configuration initEnable bit 0 = 1**; they are **not FIFOed**
(they act immediately, so a driver must not race them against queued graphics); and they
must be written as single cycles, never inside a PCI burst. Highlights per register:

- **fbiInit0** — bit 0 the external vga_pass/vga_pass_n pins (the passthrough switch,
  §3.8); bits 1/2 Chuck graphics/FIFO reset; bit 3 the register byte-swizzle enable paired
  with address bit 20; bits 10:6 the PCI-FIFO low-water mark for the stall/interrupt;
  bits 11–13 routing of LFB and texture writes into the memory FIFO and the memory-FIFO
  enable; bits 24:14/30:25 memory-FIFO water marks.
- **fbiInit1** — bit 0 the pass-through/combo strap (read-only; 0 = pass-through-only
  device); bit 1 PCI write wait state; bit 3 LFB read enable (0 at reset so a random probe
  during power-up cannot hang the bus); bits 7:4 + 24 and fbiInit6[30] the tiles-in-X
  count; bit 8 video-timing reset; bits 9–16 the HSYNC/VSYNC/blanking/DCLK output drives
  and the video clock input selects; bits 22–31 the video clocking delays and 24 BPP
  video; bit 23 scan-line interleave enable.
- **fbiInit2** — DRAM controller configuration: banking, triple-buffer compatibility bit,
  fast RAS/read-ahead turnaround, pass-through dither for 8-BPP applications, bits 10:9
  the **swap-buffer algorithm** (vsync, dac_data[0], pci_fifo_stall, sli_syncin/out),
  bits 19:11 the video buffer offset, bit 20 DRAM banking enable, bit 21 the DRAM
  read-ahead FIFO, bit 22 + 31:23 refresh enable and refresh load.
- **fbiInit3** — bit 0 the alternate triangle-register mapping; bits 5:1 video FIFO
  threshold; bit 6 TMU-interface disable; bits 10–12 the power-on straps (generic,
  VGA_PASS reset value, and the "hardcode PCI base address 0x10000000" strap); bits
  16:13/21:17 the FBI-to-Trex clock delay and Trex-to-FBI FIFO threshold; bits 31:22 the
  Y-origin swap subtraction value.
- **fbiInit4** — bit 0 PCI read wait states (0 = 1 ws, 1 = 2 ws); bit 1 LFB read-ahead;
  bits 7:2 the memory-FIFO low-water mark that triggers dumping the PCI FIFO to memory;
  bits 17:8/27:18 the memory-FIFO row start and rollover.
- **fbiInit5** — the power-on straps of the second strap bank (pci_stop, 66 MHz capable,
  dac_data width 16/24, GPIO values); bits 10:9 the **color/aux buffer allocation**
  (2C/1Z, 3C/0Z, 3C/1Z — the framebuffer partition that selects depth-buffered vs
  triple-buffered modes); the video timing/GPIO drive bits; bit 14 SLI detect; bit 15
  synced hRetrace/vRetrace reads; bits 16–21 border-color enables and scan doubling;
  bits 22, 23, 24 gamma correction for 16-bit video, HSYNC and VSYNC inversion; bit 25
  24-bit dac_data output; bit 26 interlaced video; bits 31:30 triangle-raster unit mode.
- **fbiInit6** — SLI swap counters and sync master (bits 2:0, 7:3, 8); the GPIO_2/GPIO_3/
  SLI sync/dac_rd/dac_wr/vga_pass_n pin-drive fields (each a two-bit drive/float code);
  bit 30 the tiles-in-X LSB.
- **fbiInit7** — bit 8 **CMDFIFO enable** (mutually exclusive with the memory FIFO,
  fbiInit0 bit 13); bit 9 offscreen-memory CMDFIFO store; bit 10 disable hole counting
  (software-managed depth); bits 15:11 the read-fetch threshold; bits 16/17 cross-domain
  synchronization of CMDFIFO register writes/reads; bit 18 PCI packer reset; bit 19
  chromaKey/chromaRange-to-TMU write enable; bits 26:20 the CMDFIFO PCI timeout; bit 27
  texture-write bursting across the FT bus.

The reset values that matter for a fresh bring-up are the strap-sampled ones: fbiInit1
bit 0 and fbiInit3 bits 10–12 and fbiInit5 bits 1–8 all latch the `fb_addr_a`/`fb_addr_b`
pins "at the deassertion of pci_rst" [1] §5.53, §5.55, §5.57 pp. 68–72. The values a
real driver writes are in §4.1.

### 2.8 Video timing, CLUT and external DAC

The video backend is programmed through hSync (0x220: hSyncOn 8:0, hSyncOff 26:16 in VCLK
units), vSync (0x224: vSyncOn 12:0, vSyncOff 28:16 in scanlines), backPorch (0x208:
horizontal 8:0 minus 2, vertical 24:16), videoDimensions (0x20C: xWidth 10:0, yHeight
26:16), the border registers hBorder/vBorder/borderColor and maxRgbDelta (the video
filter threshold), with the timing algebra spelled out in §13 [1] §5.44–5.51 pp. 66–67,
§13 pp. 130–131. Readback of the beam position is vRetrace (0x204, the vSyncOff counter)
and hvRetrace (0x240, both counters synchronized across the clock boundary).

The internal gamma CLUT is written through clutData (0x228: RGB in bits 23:0, index in
bits 29:24): a 33-entry × 24-bit RAM indexed by the 5 MSBs of each channel with the 3
LSBs interpolating between entries, so all stored entries must be monotonically
increasing [1] §5.67 p. 75. Writes are ignored while the video unit is held in reset
(fbiInit1 bit 8 = 1) — the opposite of the external-DAC rule below, and a documented
trap (§5).

The external RAMDAC — which on retail boards is also the clock synthesizer — is reached
through dacData (0x22C: data 7:0, register address 10:8 + 13:12, bit 11 read=1/write=0)
[1] §5.68 p. 75. The DAC's register bus is time-multiplexed with memory data, so DAC
accesses are only safe with the memory controller idle — either with the video unit in
reset or during vertical sync — and DAC **reads** require the fbiInit2/fbiInit3 address
remap (configuration initEnable bit 2 = 1), after which the latched byte comes back from
fbiInit2 bits 7:0. Which DACs exist and how they are told apart is §4.2.

### 2.9 The TMU register file

The Bruce-side file is five mode registers plus the NCC tables and palette [1]
§5.86–5.92 pp. 79–87:

- **textureMode** (0x300) — bit 0 perspective correction (tpersp_st); bits 1/2 minification/
  magnification filters; bit 3 the negative-W clamp of projected textures (forces S=T=0 when
  W < 0); bit 4 LOD dither; bit 5 NCC table select; bits 6/7 S/T clamping (else wrap by bit
  truncation); bits 11:8 the texture format — 8-bit RGB 3-3-2, YIQ 4-2-2 (NCC), Alpha,
  Intensity, Alpha-Intensity 4-4, Palette, and the 16-bit ARGB 8-3-3-2, AYIQ 8-4-2-2,
  RGB 5-6-5, ARGB 1-5-5-5, ARGB 4-4-4-4, Alpha-Intensity 8-8, Alpha-Palette 8-8 forms,
  each with its expansion to 32-bit ARGB tabulated; bits 12–29 the Texture Color Combine
  and Texture Alpha Combine Unit controls (tc_*/tca_* muxes, identical in shape to the
  FBI's combine units, with LOD and LOD_frac as extra mux sources); bit 30 trilinear
  enable; bit 31 sequential-8-bit-download addressing.
- **tLOD** (0x304) — bits 5:0 lodmin, 11:6 lodmax, 17:12 lodbias (all 4.2); bit 18 lod_odd;
  bit 19 lod_tsplit (even/odd levels split across two TMUs); bit 20 lod_s_is_wider; bits
  22:21 the aspect ratio (1:1, 2:1, 4:1, 8:1); bit 23 lod_zerofrac; bit 24
  tmultibaseaddr (use the supplemental base-address registers); **bits 25/26 tdata_swizzle
  and tdata_swap — byte and word reordering of incoming texture downloads, the texture
  half of the endianness story (§4.7)**; bit 27 raw direct texture-memory writes.
- **tDetail** (0x308) — the detail-texture clamp/bias/scale and the separate RGB/alpha
  filter selects; detail_factor = max(detail_max, ((detail_bias − LOD) << detail_scale)).
- **texBaseAddr, texBaseAddr_1, texBaseAddr_2, texBaseAddr_3_8** (0x30C–0x318) — the
  texture base in 8-byte granularity, plus the per-LOD supplemental bases selected by
  tmultibaseaddr and the active LOD [1] §5.89 p. 85.
- **trexInit0 / trexInit1** (0x31C/0x320) — Bruce hardware initialization (texture-memory
  refresh, page size, clocking). The specification punts on both: "FIXME. See Bruce spec."
  [1] §5.90–5.91 p. 85 — the Bruce specification is not in this evidence set (§6).
- **nccTable0/nccTable1** (0x324–0x380, twelve 32-bit words each) — the Narrow Channel
  Compression tables for YIQ/AYIQ textures: four Y words, four I and four Q words of
  9-bit-per-channel components; **the same address space doubles as the 8-bit texture
  palette when the MSB of the written data is 1** — a palette entry is then written as
  {1, P[7:1], R, G, B} with the palette index carried in the low bit of the I/Q register
  address, even addresses aliasing even and odd to odd [1] §5.92 pp. 85–87.

### 2.10 The 2D BitBLT engine

A self-contained 2D blitter, with state independent of the 3D pipeline [1] §1 p. 7,
registers at 0x2C0–0x2FC: source/destination base addresses and XY strides, source and
destination chroma-range registers, 2D clip registers, bltSrcXY/bltDstXY/bltSize,
the 16-entry bltRop, bltColor (foreground/background for monochrome expansion), and
bltCommand/bltData. bltCommand (0x2F8) carries the command select (bits 2:0:
screen-to-screen, CPU-to-screen, rectangle fill, SGRAM fill), the CPU source format
(bits 5:3: two monochrome forms, 16 BPP, and three 24 BPP forms with 2×2/4×4 dither),
the source lane order (bits 7:6), byte swizzle and word swap of CPU source data
(bits 8/9 — the blitter's endianness path), chroma-range and clipping enables, tiled
memory selects for source and destination (bits 15:14, the 32×32 tile algorithm), and
bit 31, the launch bit; bltDstXY bit 31 and bltSize bit 31 also launch [1] §5.93 p. 87
ff. The destination is always 16 BPP 5-6-5; monochrome sources expand 32 pixels per word
against bltColor [1] §5.93 pp. 87–91.

### 2.11 The CMDFIFO registers

The command-FIFO transport is managed by seven registers (0x1E0–0x1F8): cmdFifoBaseAddr
(base and end pages of the circular buffer in framebuffer memory), cmdFifoBump (the
software "N words added" doorbell), cmdFifoRdPtr, cmdFifoAMin/cmdFifoAMax (the
out-of-order address window), cmdFifoDepth and cmdFifoHoles — the last two read-only
reflections of the hole-counting state machine [1] §5.60–5.66 pp. 74–75, §11.3 pp.
122–124. Like the fbiInit registers they are not FIFOed and must not be written in a
PCI burst. The packet formats they transport are §3.1.

### 2.12 PCI configuration space

The configuration header is a standard type-0 layout with a 3dfx tail [1] §6 pp.
102–107:

| Config offset | Register | Notes |
|---|---|---|
| 0x00 | Vendor_ID | 0x121A, read only |
| 0x02 | Device_ID | 0x0002 on shipping hardware (§1.4); the spec text prints SST-1's 0x1 [1] §6.2 |
| 0x04 | Command | bit 1 (memory access enable) is the only writable bit; the rest read as configured by the fb_addr_a[5] strap |
| 0x06 | Status | DEVSEL timing and fast-back-to-back from straps; 66 MHz capable from fb_addr_b[1] |
| 0x08 | Revision_ID | **always 0x2** "for software backwards compatibility with Voodoo Graphics"; the true Voodoo2 revision lives in initEnable bits 15:12 |
| 0x09 | Class_code | 0x038000 (display controller, non-VGA) when fb_addr_a[6]=0, or 0x040000 (video multimedia device) when 1 |
| 0x0C–0x0F | Cache_line_size, Latency_timer, Header_type, BIST | all hardwired to 0 (the device never masters; header type 0, single function) |
| 0x10 | memBaseAddr | the one BAR: bits 31:24 writable, all others read-only; bit 3 = 1 (prefetchable). Default 0xFF000008, or 0x10000008 when fb_addr_b[1]=1; writing 0xFFFFFF resets it for size probing |
| 0x3C/0x3D | Interrupt_line / Interrupt_pin | pin hardwired 1 = INTA#; line R/W, default 0 |
| 0x3E/0x3F | Min_gnt / Max_lat | hardwired 0 — no bus mastering |
| 0x40 | initEnable | see below |
| 0x44/0x48 | busSnoop0 / busSnoop1 | write-only snooping address match registers; read as 0 |
| 0x4C | cfgStatus | alias of the memory-mapped status register, readable without touching the aperture |
| 0x50 | cfgScratch | free scratchpad |
| 0x54 | siProcess | the silicon process monitor: ring-oscillator and PCI counters |
| 0x58–0xFF | reserved | |

**initEnable** (0x40) [1] §6.16 pp. 106–107 — the register that unlocks everything:

| Bit | Meaning |
|---|---|
| 0 | enable writes to the fbiInit registers (default 0) |
| 1 | enable writes to the PCI FIFO — "must be set for normal Voodoo2 Graphics operation" (default 0) |
| 2 | remap {fbiInit2, fbiInit3} to {dacRead, videoChecksum} — the DAC-read path of §2.8 |
| 4–9 | bus-snoop enables and match types for snooping registers 0 and 1 (enable, memory/IO, write/read) |
| 10–11 | SLI PCI-bus ownership and master/slave determination |
| 15:12 | the secondary (true) revision ID |
| 19:16 | fab identification, read only |
| 20 | PCI interrupt enable — latched from the fb_addr_a[7] strap; without it no interrupt leaves the chip |
| 21 | PCI interrupt timeout enable (deassert INTA# ≥ 32 PCI clocks between interrupts) |
| 22 | NAND-tree test enable |
| 23 | SLI address snoop enable (with bits 31:24 as the snoop address) |

**busSnoop0/1** implement the "make sure VGA passthrough does not drive the monitor on
reset" service: when enabled, a PCI cycle whose type and address match the programmed
value sets the vga_pass pin [1] §6.17 p. 107 — the hardware mechanism behind a glitch
every pass-through card must survive (§3.8).

## 3. Behaviour

### 3.1 The PCI slave, its FIFOs and command transport

The slave interface decodes memory cycles against memBaseAddr bits 31:24, supports
zero-wait-state and burst writes, and posts writes into an asynchronous 128-entry FIFO
"which allows sufficient write posting capabilities for high performance" — asynchronous
to the graphics engine, so the memory interface can run at full speed regardless of PCI
clock [1] §3.3 p. 15. (The status register's 6-bit free-space field counts a 64-entry
host FIFO; the specification's two numbers describe the same front end at different
points, and neither document reconciles them — §6.)

Behind the PCI FIFO, optionally, sits the **memory FIFO**: framebuffer memory loaned to
the transport, programmable via fbiInit0 bits 11–13 and fbiInit4, so "up to 65536 host
writes can be queued without stalling the PCI interface" [1] §3.3 p. 15. Its occupancy
is the status register's bits 27:12.

The CMDFIFO is a third, incompatible mechanism: a command buffer built in off-screen
framebuffer memory (or in the on-chip FIFOs), which the chip parses and executes. Writes
into the 2 MB CMDFIFO aperture are packetized — packet type 0 jumps/calls/returns
(including a two-word AGP form the chipset never uses on a Mac), type 1 bursts a counted
block of register writes to one base or consecutive addresses, type 2 writes a mask of
the 2D register block, type 3 the 3D "immediate" block, and so on [1] §11.3.3–11.3.8
pp. 123–127. Management is either software ("bump" the depth register after the CPU's
writes are flushed) or hardware (the accelerator snoops the buffer addresses and counts
holes; the aMin/aMax/holeCount dance is specified step by step [1] §11.3.1.2 p. 123).
When the CMDFIFO map is selected, direct writes to any register outside the privileged
set (fbiInit, intrCtrl, video timing, dacData, cmdFIFO control) are accepted by the PCI
slave but dropped — the trapdoor of §5 [1] §11.2 p. 121.

Whether the Mac driver ever uses the CMDFIFO is not directly evidenced either way; the
Mac Glide library contains the full CMDFIFO setup machinery (the string "sst1InitCmdFifo
(): Disabling Command Fifo..." is present in the shipped GlideLib2.x [8]) but nothing
observed here records it being enabled in a Mac session (§6).

### 3.2 The pixel pipeline

Every rendered pixel walks one path, in this order: texture lookup (Bruce), chroma-key
and chroma-range compare (immediately after lookup, before lighting, fog or blending —
and alpha is ignored in the compare [1] §5.28 p. 60), the color and alpha combine units,
the fog unit (table lookup on the MSBs of normalized 1/W, 8 fractional bits blending
between entries, optional dithering of the factor), the alpha function (compare against
alphaMode bits 31:24, fail = pixel invalidated and fbiAfuncFail incremented), alpha
blending (independent RGB and alpha-channel factors, with the 0xF asymmetry: source
ASATURATE for polygonal anti-aliasing in front-to-back order, destination
color-before-fog for multi-pass atmospherics), depth buffering (integer Z iterator, or
a floating-point W/Z scheme with 4-bit exponent and 12-bit inverted mantissa that
reuses the integer comparisons), the stipple mask (rotate or 4×8 pattern mode), the
write masks, and finally dithering — 24-bit source colors reduced to the 16-bit buffer
by 4×4 or 2×2 ordered dither or plain truncation [1] §5.17–5.21, §5.28–5.31. The five
counters fbiPixelsIn/fbiChromaFail/fbiZfuncFail/fbiAfuncFail/fbiPixelsOut count the
outcomes; all are cleared by nopCMD bit 0 (fbiTrianglesOut by bit 1) and reset only by
power-on [1] §5.34–5.39 pp. 62–63.

### 3.3 Triangle rendering

A TRIANGLE command consumes the vertex and parameter registers, sorts the vertices by Y,
walks the minor edge pair (AB, BC) against the major edge (AC), and shades with the
signed-area convention: bit 31 of the triangleCMD write selects counter-clockwise
(0, positive area) or clockwise (1) [1] §5.3, §5.16 pp. 31, 36. Sub-pixel correction to
0.4 × 0.4 is performed by the setup unit when fbzColorPath bit 26 is set, and it mutates
the parameters on the way in: "the exact data sent from the host CPU is changed to
account for subpixel alignments. If a triangle is rendered with subpixel correction
enabled, all subsequent triangles must resend starting color, depth, and texture
coordinate parameters" — otherwise the previous triangle's corrected values are
corrected again [1] §12.4 p. 128.

Strips and fans go through the setup engine: write a vertex, close with sDrawTriCMD;
rendering of one triangle overlaps the download of the next (the specification's worked
example sends R, D1, D2 before triangle 1 even starts) [1] §5.70 pp. 76–77. Culling
(bit 17 of sSetupMode, sign in bit 18) drops back faces in the setup unit itself.

The FASTFILL command is the clear: it fills the clip rectangle with color1 (optionally
dithered) and zaColor's depth word, honoring only the fbzMode write masks and draw-buffer
select and nothing else — no depth test, no alpha, no blending [1] §7.3 p. 108, §5.24
p. 58. The clip rectangle is inclusive of left/lowY and exclusive of right/highY [1]
§5.24 p. 58.

### 3.4 Linear framebuffer access

The LFB window is a per-pixel door into the color and depth buffers with a fixed
geometry: "all linear frame buffer accesses assume a 1024-pixel logical scan line width",
2048 bytes per line in 16-bit formats and 4096 in the 32-bit ones, with the Y origin
selected by fbzMode bit 16 (and, for reads and bypassed writes, lfbMode bit 13) [1] §9
pp. 114–116. Writes are formatted by lfbMode (§2.5); two 16-bit pixels pack into one
longword with the packing order chosen by lfbMode bit 11, and the byte-swizzle/word-swap
ordering rule is fixed: swizzle first, word-swap second, lane-select last [1] §5.21
pp. 55–56. Writes can bypass the pixel pipeline entirely (lfbMode bit 8 = 0) — then the
fbzMode masks are ignored, only the buffers the format names are touched, and the depth
value comes from zaColor — or run through it (bit 8 = 1), becoming subject to clipping,
dithering, alpha, depth, chroma, fog and masking like any rendered pixel [1] §5.21.1
pp. 53–56.

Reads are always 16-bit, two pixels per longword, front/back/depth buffer selected by
lfbMode bits 7:6 — and they are expensive by design: "reads from the linear frame buffer
bypass the PCI host FIFO... but are blocking", so the read stalls until the FIFO drains
and the pipeline flushes; the specification advises idling the chip first [1] §9.2
p. 115. Off-screen writes with clipping disabled are undefined behavior; the Linux
driver notes they "just wrap and read/print pixels on screen. Ugly but not that
dangerous" [7] `sstfb.c`.

### 3.5 Texture memory

The 8 MB texture window is **write-only** — reads return undefined data, and the only
way to read a texel back is to render it into the framebuffer, accepting dithering
[1] §10 p. 116. Downloads are 32-bit writes; two 16-bit or four 8-bit texels per word,
with inhibited lanes for narrow maps, and the address itself encodes (TREX, LOD, T, S)
fields, right-aligned for maps smaller than 256×256; the sequential-8-bit-download mode
re-lays the S field for byte-streaming [1] §10 pp. 116–119.

Textures are stored **as if mipmapped even when they are not** [1] §10 p. 116: texbaseaddr
points at where LOD 0 (the 256×* level) would start, and the usable levels sit above it.
The level-size table (16-bit formats, in 8-byte units: LOD 0 of a square map is 2^14
units, halving per level; 8-bit formats are half) plus three worked examples — including
the wrap-below-zero trick where texbaseaddr subtracts unused lower levels so that the
loaded levels alias contiguously — are given in the specification [1] §10 pp. 116–118.
Two rules matter to any packing algorithm: a texture cannot span two banks (each bank
has one RAS), and the level's size must be known to compute the next free start address.

### 3.6 Video timing and the output path

The video backend generates its own sync: hSyncOn/hSyncOff in dot clocks, vSyncOn/
vSyncOff in scanlines, back porches in each unit, borders on four edges, with the
front porches inferred from the totals [1] §13 pp. 130–131. The frame buffer feeds a
33-entry interpolated gamma CLUT (§2.8), whose output goes to the external RAMDAC over
`dac_data` — 16 bits double-pumped, or a full 24-bit port when fbiInit5 bit 25 is set
[1] §5.57 p. 71. Interlace, scan-doubling and sync polarity are all fbiInit5/6 selects;
the video output pins can be tri-stated, which is how SLI slaves and headless
configurations keep off the monitor lines [1] §5.53 p. 68, §5.58 p. 72.

### 3.7 Interrupts

Six interrupt sources — hsync rising/falling, vsync rising/falling, PCI FIFO full, and
the software USERINTERRUPT — are masked in intrCtrl bits 5:0 and flagged in bits 11:6
[1] §5.2 p. 30. The USERINTERRUPT command deserves care: a write to userIntrCMD
generates the interrupt (bit 0), optionally stalls the graphics engine until software
clears it (bit 1 = 1 — the only race-free way to use it, since unstalled user interrupts
overwrite each other's tag), and carries an 8-bit tag that survives into intrCtrl bits
19:12; writes with intrCtrl bit 5 clear are simply "dropped" [1] §5.30 p. 61, §7.5
p. 109. On the card the interrupt is INTA# (configuration Interrupt_pin = 1); the
Power Macintosh routes it through the PCI interrupt fabric of Grand Central
([grand-central.md](../../../machines/tnt/grand-central.md) §2.2) — nothing about the card
is Mac-specific there, and the shipped Mac drivers drive 3D entirely by polling the
status register [8] (no interrupt setup appears anywhere in the driver set's symbol
strings beyond the generic Glide library code).

### 3.8 Passthrough, bus snooping and the video takeover

The vga_pass pins (fbiInit0 bit 0) are the mechanical heart of a pass-through board:
with the bit clear the 2D card's analog output flows through the Voodoo2's DAC socket to
the monitor; with it set the Voodoo2's own video backend drives the monitor [1] §5.52
p. 67. The Glide API exposes this as `grSstControlMode(GR_CONTROL_ACTIVATE)` /
`GR_CONTROL_DEACTIVATE` — "grSstControlMode determines whether the VGA display or
Voodoo Graphics display is visible" [4] §grSstControlMode p. 67 — and the Mac driver
carries the same machinery (the strings `.CheckSwapPassthru`, `.pciSetPassThroughBase`
and `sst1InitVgaPassCtrl` are all present in the shipped GlideLib2.x [8]).

Two reset-time hazards get dedicated hardware. First, the vga_pass reset value itself is
a power-on strap (fb_addr_a[4]), mirrored read-only in fbiInit3 bit 11 [1] §5.55 p. 70.
Second, the busSnoop registers watch the PCI bus for a specifically shaped cycle — a
VGA register access by type and address — and force vga_pass on when they see one, so
that "VGA passthrough capability does not drive the video monitor upon soft and hard
resets"; the snoop never interferes with the snooped cycle itself [1] §6.17 p. 107. The
Glide init code sets both snoop registers to a default match value at bring-up [6]
`sst1InitRegisters` (SST_PCI_BUS_SNOOP_DEFAULT).

### 3.9 Buffer swaps, triple buffering and SLI

SWAPBUFFER is a FIFOed command like any other, which is the whole point: "software does
not have to poll and wait for vertical retrace to manually swap buffers — this frees the
CPU to perform other functions while the graphics engine automatically waits for vertical
retrace" [1] §7.4 p. 108. The swap may be retrace-synchronized (bit 0), delayed by an
interval of retraces (bits 8:1, for frame-rate governing), or turned into a pure
retrace-wait without swapping (bit 9) [1] §5.25 p. 59. Pending swaps are visible in
status bits 30:28, and Glide exposes them as `grBufferNumPending()` — "The maximum value
returned is 7, even though there may be more buffer swap requests in the queue" [3]
Ch. 3 §"Swapping Buffers".

With the framebuffer partitioned for three color buffers (fbiInit5 bits 10:9), a swap
does not block: the engine moves to the third buffer and queues the front-pointer change
for the next retrace; up to two fully rendered buffers may stand queued. The constraints
are stated exactly once and are easy to miss: triple buffering requires retrace sync on
and interval 0, and swapbufferCMD bit 9 must be 0 [1] §5.25 p. 59.

fbiSwapHistory (0x258) keeps the frame-rate evidence: after each swap the register shifts
its nibbles left and records the number of retraces since the previous swap (saturating
at 0xF) — the last eight frames' swap spacing [1] §5.40 p. 64.

Scan-line interleaving runs two complete boards on alternate scanlines of one image.
On the PCI side it is configuration initEnable bits 10–11 (which board owns the bus;
master/even vs slave/odd), the SLI address snoop (bit 23 + bits 31:24) that lets the
slave observe the master's register writes, and fbiInit1 bit 23; on the board side,
the sli_syncin/sli_syncout pins and the swap algorithm select fbiInit2 bits 10:9 = 3
[1] §5.53–5.58, §6.16. Glide's init disables SLI when it finds it (to avoid PCI
contention) unless the configuration asks otherwise [6] `sst1InitMapBoard`. Retail Mac
cards never shipped in SLI pairs — one PCI card, one DB-15 loop, no companion port
(*inferred*; neither card page documents an SLI connector, and the Mac market's single
2D pass-through topology leaves no room for one) —
but the chipset and the driver both carry the machinery, and the Mac Glide library
contains the SLI clock-delay variables (SSTV2_SLIM_*/SSTV2_SLIS_*) [8].

### 3.10 The 2D BitBLT engine

The blitter is a separate engine with its own register block (§2.10) and no interaction
with the 3D state: screen-to-screen copies at half a pixel per clock (the source and
destination share the memory port), CPU-to-screen expansion at a pixel per clock,
rectangle fills at a pixel per clock, and the SGRAM color-expansion fill at sixteen
pixels per clock — the 1.2 GPixels/s clear figure of §1.2 [1] §8 pp. 110–113. The four
commands and their register dependencies are tabulated in the specification's §8.5–8.6
[1] pp. 113–114. Screen-to-screen BLTs may overlap and choose direction by the sign of
bltSize; CPU-to-screen BLTs may not (negative sizes are unsupported) [1] §8.1–8.2
pp. 110–112. Whether anything in the Mac software stack ever drives this engine is not
established — the Mac driver's symbol strings expose the Glide LFB API (grLfbLock/
grLfbUnlock/grLfbWriteRegion [8]) rather than the blit commands; Glide 2.x itself
performs its 2D work through the LFB [3] Ch. 11.

## 4. Programming model

### 4.1 Power-on bring-up

The register-level bring-up below is the open-sourced Glide init module's
(`sst1InitRegisters`, CVG variant) [6]; the Linux framebuffer driver performs the same
shape of sequence [7] `sstfb_set_par`. Order matters throughout, and the specification's
programming caveats [1] §12 p. 128 are the annotations:

1. **Find the card**: scan PCI configuration space for vendor 0x121A / device 0x0002
   (overridable, and on the Mac parameterized by the Name Registry, §4.6); read the BAR
   (probing it first with 0xFFFFFFFF if needed), map the 16 MB aperture, and set the
   configuration Command register's memory-access bit — "Must be set for PnP BIOS which
   do not enable memory mapped accesses" [6].
2. **Unlock and reset**: write initEnable = init-write-enable (bit 0) — not the FIFO bit
   yet; reset the snoop registers to their defaults; then, in strict order: set the
   video-timing reset (fbiInit1 bit 8), set the Chuck graphics and PCI-FIFO resets
   (fbiInit0 bits 1–2), drop DRAM refresh (fbiInit2 bit 22), idle, then release the
   PCI FIFO reset and release the graphics reset — the Glide source comments the last
   step "THIS MUST BE PRESENT OTHERWISE THE PCI FIFO WILL NEVER DRAIN!" [6]. The
   specification's matching caveat: video must be reset *before* graphics, "otherwise
   video unit could potentially hang waiting for the graphics unit to respond" — that
   ordering is in the fbiInit1/fbiInit0 sequence above [1] §5.53 and [6].
3. **Write the init registers to their defaults**: fbiInit0 = 0x00000410 (with the
   texture- and LFB-memory-FIFO enables set, "or else texture memory detection will hang
   on some machines" [6]), fbiInit1 = 0x00201102, fbiInit2 = 0x80000040, fbiInit3 =
   0x001E4000 (texture mapping initially *disabled*, so a hung TMU cannot poison the
   FBI; the Trex-to-FBI FIFO threshold is programmed at the same time), fbiInit4 =
   0x00000001, fbiInit5 = synced retrace reads | 16-bit gamma correction | GPIO_1 select,
   fbiInit6 = 0, fbiInit7 = texture-write burst enable | TMU chroma-register write
   enable [6] `sst1init.h`.
4. **Enable the FIFO**: initEnable |= bit 1 (PCI-FIFO write enable). From here on,
   ordinary register writes are queued, and the non-FIFOed registers (fbiInit, intrCtrl,
   video timing, dacData, cmdFifo) must not be written while the engine is busy or
   inside a burst [1] §5.52–5.59.
5. **Detect the DAC and program the clocks** (§4.2), then the video mode (§4.3), then
   enable memory accesses normally.

The idling discipline used between every step is the specification's own recipe: write
a NOP, then poll status — "always issue a NOP command before reading the status register
when polling on the CVG busy bit", and "always read CVG inactive in status three times"
before declaring the machine idle, because of "a potential deadlock condition between
internal CVG state machines" after LFB accesses; the sample `CVG_IDLE()` loop is printed
in the caveats section [1] §12.3 p. 128.

### 4.2 DAC detection and clock programming

Voodoo2 boards carry an external RAMDAC whose registers are reached through dacData, and
the driver cannot assume which part is fitted: the Glide init probes three families in
order — ICS, AT&T, TI (or a data-file-described "INI" DAC on arcade boards) [6]
`sst1InitDacDetect`, [7] `sstfb.c`. The detection sequence is concrete and citable: set
the fbiInit2/3 address-remap (initEnable bit 2), which also routes DAC reads to fbiInit2
bits 7:0 (§2.8), read the DAC's manufacturer and device ID registers, and match —
AT&T parts identify with MIR = 0x84, TI with MIR = 0x97 (both DIR = 0x09) [6]
`sst1init.h` (SST_DACREG_INDEX_MIR/DIR_ATT/TI_DEFAULT), [7] `sstfb.c` "supposed to detect
AT&T ATT20C409 and Ti TVP3409 ramdacs". The ICS part is identified differently, by
reading its PLL registers back and comparing against the power-on values (f1 = 0x55,
f7 = 0x71, fB = 0x79) [6], [7].

The same DACs carry the PLLs for both the video dot clock and the graphics clock, so
"clock programming" is a DAC conversation: Glide's gamma module and the driver's PLL
calculator (M/N/P) program the ICS's CLK0/CLK1 registers or the ATT/TI equivalents,
then the driver bumps the graphics clock by measurement against the silicon process
monitor [6] `sst1InitSetGrxClk`, `sst1InitCalcGrxClk`. The clock fine-tuning variables
persist into every driver: the SSTV2_*IN/VOUT_CLKDEL family exists precisely because
the FBI-to-TMU and TMU-to-framebuffer bus timing had to be tuned per board [6], [8].

### 4.3 A real mode-set

The Linux driver's `sstfb_set_par` is the cleanest published end-to-end mode-set [7],
and it follows the specification's timing algebra exactly:

```
write nopCMD = 0;                    poll status until idle
cfg.initEnable = EN_INIT_WR
set fbiInit1 VIDEO_RESET, fbiInit0 FBI_RESET|FIFO_RESET, clear fbiInit2 EN_DRAM_REFRESH
backPorch    = vBackPorch << 16 | (left_margin - 2)
videoDimensions = yDim << 16 | (xres - 1)
hSync = (hSyncOff - 1) << 16 | (hsync_len - 1)
vSync = vSyncOff << 16 | vSyncOn
cfg.initEnable = EN_INIT_WR | REMAP_DAC ; dac.set_vidmod ; dac.set_pll ; restore
fbiInit1 |= output-drives | tiles-in-X bits | vclk select
fbiInit6 = tiles-in-X LSB ; fbiInit5 |= interlace/doublescan/sync-polarity
release VIDEO_RESET, FBI_RESET, FIFO_RESET; set EN_DRAM_REFRESH
cfg.initEnable = EN_FIFO_WR
lfbMode = 5-6-5 format, front buffer, pipeline off
```

The driver's own validity limits encode the register field widths — Voodoo2 modes cap
at xres ≤ 2048 (2^11) and yDim < 2048, tiles-in-X < 64, and the tile count is computed as
two 32-pixel tiles per 64 pixels ("voodoo2 has 32 pixel wide tiles, BUT strange things
happen with odd number of tiles" [7]) — matching the six-bit tiles-in-X field of §1.5.
Note the driver leaves clipping off and then warns about it: off-screen LFB writes are
"undefined (_very_ undefined)" [7].

### 4.4 The Glide contract

Glide is the native API, and the Voodoo2 register file is essentially Glide's public
state flattened into longwords. The application-side contract [3] Ch. 3: call
`grSstQueryBoards()` before anything (it "does not change the state of any hardware"),
`grGlideInit()`, `grSstQueryHardware()`, `grSstSelect()`, then `grSstWinOpen(screen
resolution, refresh, color format, origin, num_buffers, num_aux_buffers)` — after which
the framebuffer is front/back/(aux) buffers, the aux buffer is depth, alpha or the third
color buffer (mutually exclusive; contention is a debug-build diagnostic only), and the
origin selects the top-left or bottom-left Y convention of §3.4 [3] Ch. 3 §"The Display
Buffer". `grRenderBuffer` picks the draw buffer; `grBufferClear` issues the FASTFILL of
§3.3; `grBufferSwap(interval)` queues the SWAPBUFFER of §3.9; `grBufferNumPending`
reads status bits 30:28; `grSstVRetraceOn`/`grSstVideoLine` read the beam position
[3] Ch. 3. Texture management (Chapter 10) is the mipmap packing of §3.5 dressed in an
allocator; linear framebuffer access (Chapter 11) is the lfbMode machinery of §2.5
behind `grLfbLock`/`grLfbWriteRegion`.

Voodoo2 requires Glide 2.5 or later — the shipped Mac library registers a Glide 2.x
CFM fragment and the driver Read Me's own fix notes ("Bugs with downloading very small
mipmap levels in Glide2.x and Glide3.x have been fixed") show the same code base serving
both [8].

### 4.5 The Mac driver stack

The Mac software stack is four files, dragged into the Extensions folder, delivered by
3dfx as a beta [8] [9]:

| File | Role |
|---|---|
| `3dfx GlideLib2.x` | the Glide 2.x shared library (CFM, `shlb`): PCI discovery, hardware init (the full `sst1Init*` machinery is present in its symbol strings), the gr* API |
| `3Dfx GlideLib3.x` | the Glide 3.x shared library (the OpenGL renderer sits on this one) |
| `3dfx Rave` | the QuickDraw 3D RAVE engine shim, built on GlideLib2.x; registers the engine name **"3Dfx SST-1"** |
| `3dfx OpenGL Renderer` | the fullscreen-only preliminary OpenGL renderer, built on GlideLib3.x |

All four were observed by opening the beta 5 archive; the "Voodoo2 Read Me" documents the
file set and the install procedure ("drag the files in the 'Into Extensions Folder'
folder into the Extensions folder... You may also drag the three files onto your System
Folder icon") [8]. The stack is pure user-space CFM shared libraries — **no 'ndrv'
driver and no extension of the classic device-driver kind appears in the archive**;
there is no interrupt extension, no Name Registry driver matching, nothing in the ROM.
The RAVE shim's own imports name the QuickDraw 3D accelerator interface and GlideLib2.x;
its engine device-check, gestalt and get-method entry points carry the `tnslEngine*`
prefix of the RAVE engine interface [8] (symbol strings).

The requirements and the known-bad list are the Read Me's [8]:

- System 8.1 or later; a PowerPC with at least 32 MB of RAM; any PCI Voodoo2 card
  (iMac cards not supported).
- "There appears to be some stability issues with the Voodoo2 drivers and MacOS 9.
  We are trying to isolate the problem, but have thus far been unable to reproduce it
  at 3dfx."
- The device is "a fullscreen only device": windowed-OpenGL games cannot work, "Quake2
  and Quake3 are known to work, however."
- RAVE coverage is partial: "Nanosaur and Bugdom appear to be missing some graphics...
  These games, and others use features of RAVE that are not implemented by the Voodoo2
  drivers."
- Beta 3 added the OpenGL renderer; beta 4 fixed an OpenGL single-texture slowdown;
  beta 2 fixed "the 'black surfaces' problem in games such as Quake3 and Unreal" (very
  small mipmap downloads).

The companion "OpenGL Notes" adds the two most Mac-characteristic driver facts [8]:
Quake 3's gamma adjustment "is not compatible with non-integrated 2D/3D boards such as
the Voodoo2" — the fix is a `voodoo2.var` file containing `[VOODOO2]` and
`SSTV2_GAMMA=1.3` — and Rainbow Six's cursor trails come from assuming "double
buffering is done via back buffer to front buffer blits, rather than page flipping",
which the Voodoo2's real page flipping does not match.

### 4.6 Discovery on the Macintosh

Because the card has no ROM, the Mac driver owns discovery end to end. The GlideLib2.x
import table names the mechanism exactly [8]: the library imports `NameRegistryLib`
(`RegistryEntrySearch`, `RegistryEntryIterate*`, `RegistryPropertyGet*`), `PCILib` and
the Expansion Bus Manager's configuration accessors (`ExpMgrConfigReadByte/Word/Long`,
`ExpMgrConfigWriteByte/Word/Long`). So the contract is: walk the Open Firmware Name
Registry for the PCI node — the ROM's device tree carries the card as a child of its
Bandit bridge with `assigned-addresses` naming the 16 MB aperture
([bandit.md](../../../machines/tnt/bandit.md) §4.1–4.2; the window itself is
[tnt.md](../../../machines/tnt/tnt.md) §3.3) — match vendor/device ID, then perform all
configuration-space access through the Expansion Bus Manager rather than raw memory
writes [8]. The library's internal symbols show the same flow (`pciFindCardMulti`,
`pciMapCard`, `sst1InitMapBoard`, `.SstSetupMac`, `sst1InitDacDetectATT/TI/ICS`,
`sst1InitVgaPassCtrl` [8]) — the PC init code, re-front-ended for the Name Registry.

What Open Firmware itself contributes is only the enumeration of §1.4: address space
allocated, no driver, no name property [5] p. 88. A ROM-less card is why the Mac driver
must scan, and also why the card can never be caught early: nothing runs before the
Extensions folder loads.

### 4.7 Endianness

The chipset meets a big-endian host with three explicit mechanisms, and the Power
Macintosh needs all of them [1] §5.21 p. 56:

1. **Register accesses**: fbiInit0 bit 3 arms the address-bit-20 swizzle; once armed,
   any register access with address bit 20 set swaps bytes 3↔0 and 2↔1 of the data on
   the way in and out [1] §5 p. 21.
2. **Linear framebuffer writes**: lfbMode bit 12 byte-swizzles each incoming longword
   (bits 31:24 ↔ 7:0, 23:16 ↔ 15:8) before anything else — "for big endian CPUs...
   PowerPC processors should enable byte swizzling" — with lfbMode bit 11's word swap
   and the lane select applied afterward in a fixed order, and lfbMode bit 15 covering
   reads [1] §5.21 pp. 55–56.
3. **Texture downloads**: tLOD bits 25/26 (tdata_swizzle, tdata_swap) reorder each
   incoming texture word, byte swizzle first and short swap second [1] §5.87 p. 83.

The Linux driver's big-endian port uses exactly this: "Enable byte-swizzle
functionality in hardware. With this enabled, all our read- and write-accesses to the
voodoo framebuffer can be done in native format, and the hardware will automatically
convert it to little-endian" [7] `sstfb.c` (the `__BIG_ENDIAN` branch sets both the
word- and byte-swizzle bits on reads and writes). The Mac driver walks the same
registers (the Mac GlideLib exports `guEndianSwapBytes`/`guEndianSwapWords` alongside
`grLfbWriteColorSwizzle` [8]), but with what split between hardware swizzling and
software conversion per color format is not directly evidenced — the exported helper
names prove both paths exist in the API (§6).

### 4.8 The configuration variables

The init code reads a large set of `SSTV2_*` environment variables — the escape hatches
for boards that do not match the defaults. The list below is read straight out of the
shipped Mac GlideLib2.x's string table [8] and matches the open-source init module's
documented set [6]:

| Variable | Knob |
|---|---|
| SSTV2_DEVICEID, SSTV2_BOARDS, SSTV2_SWAPBOARDS | which PCI device to claim, multi-board ordering |
| SSTV2_FBICFG, SSTV2_TMUCFG, SSTV2_NUM_TMUS | explicit board configuration overrides |
| SSTV2_FBI_MEMSIZE, SSTV2_TMU_MEMSIZE | memory sizes (instead of detection) |
| SSTV2_GRXCLK, SSTV2_VIDCLK2X | graphics and video clocks |
| SSTV2_SCREENREZ, SSTV2_SCREENREFRESH, SSTV2_DIMENSIONS, SSTV2_REFRESH_640x480 / _800x600 / _1024x768 / _960x720 / _640x400 / _512x384 | video mode and the per-resolution refresh tables |
| SSTV2_VSYNC, SSTV2_HSYNC, SSTV2_BACKPORCH | raw timing overrides |
| SSTV2_GAMMA, SSTV2_RGAMMA/GGAMMA/BGAMMA | gamma (the Quake 3 workaround of §4.5 sets SSTV2_GAMMA via `voodoo2.var`) |
| SSTV2_FT_CLKDEL, SSTV2_TF0/1/2_CLKDEL, SSTV2_PFT/PTF0/1/2_CLKDEL | FBI↔TMU bus clock delays (preliminary and final values) |
| SSTV2_TF_FIFO_THRESH, SSTV2_VFIFO_THRESH, SSTV2_PCIFIFO_LWM | FIFO thresholds |
| SSTV2_MEMFIFO, SSTV2_MEMFIFO_LWM/HWM/ENTRIES, SSTV2_MEMFIFO_LFB/TEX, SSTV2_MEMOFFSET | the memory FIFO |
| SSTV2_CMDFIFO_DIRECT, SSTV2_CMDFIFO_NOHOLES | CMDFIFO management mode |
| SSTV2_VGA_PASS, SSTV2_SLIDETECT, SSTV2_SLISWAP, SSTV2_SLIM_*/SSTV2_SLIS_* | passthrough and SLI behavior |
| SSTV2_VIDEO_24BPP, SSTV2_VIDEO_CLEARCOLOR, SSTV2_VIDEO_DISABLE, SSTV2_VIDEO_NOCLEAR, SSTV2_VIDEO_FILTER_* | video backend |
| SSTV2_IGNORE_INIT_REGISTERS, SSTV2_IGNORE_INIT_VIDEO, SSTV2_IGNORE_IDLE, SSTV2_IGNORE_CACHING, SSTV2_IGNORE_CLKDELAYS, SSTV2_NOREMAP, SSTV2_NOSHUTDOWN, SSTV2_TEXMAP_DISABLE, SSTV2_MDETECT, SSTV2_SLOWPCIWR, SSTV2_FASTPCIRD, SSTV2_SLOWMEM_RTW/WTR, SSTV2_FASTMEM_RAS_READS | bring-up and bring-down debug/compat switches |

The Mac driver reads the same names from a `voodoo2.var` file — the mechanism the OpenGL
Notes prescribe for the Quake 3 gamma fix, with the file carrying an `[VOODOO2]` section
in the format of the init module's INI parser [6] `voodoo2.ini`, [8].

## 5. Quirks & errata

- **The Device_ID specification text is wrong.** The Voodoo2 spec prints the SST-1
  device ID ("Default is 0x1", copied verbatim from [2]); the shipping device answers
  0x0002, and every driver matches on 0x0002 [6] [7]. A PCI enumerator keyed to the
  spec's number finds nothing.
- **Revision_ID is always 2, deliberately.** The configuration Revision_ID returns 0x2
  "for software backwards compatibility with Voodoo Graphics"; the real silicon revision
  hides in initEnable bits 15:12 [1] §6.5 p. 103.
- **The class code is a strap.** fb_addr_a[6] chooses between display-controller
  (0x038000) and video-multimedia (0x040000) class codes at reset [1] §6.6 p. 104 — two
  nominally identical cards can enumerate differently.
- **Reads always come from Chuck.** The chip-select field is a write-only bitmap; a read
  at any chip-select returns Chuck's data, and Bruce-resident registers read as
  undefined [1] §5 p. 22.
- **32-bit accesses only, everywhere** — registers, CMDFIFO and texture memory; the LFB
  additionally allows 16-bit accesses by format. Byte writes are illegal in all three
  spaces [1] §12.2 p. 128.
- **The non-FIFOed registers are a race.** fbiInit*, intrCtrl, the video timing
  registers, dacData and the cmdFifo registers act immediately, may not be written in a
  burst, and can overtake queued graphics writes [1] §5.52–5.59.
- **CMDFIFO mode silently drops most register writes.** With fbiInit7 bit 8 set, writes
  to any register outside the privileged list are "accepted by the PCI slave
  controller, but... effectively 'dropped'" [1] §11.2 p. 121.
- **NOP before every status poll, and idle must be read three times.** The chip has a
  known deadlock window after LFB accesses; the CVG_IDLE recipe is a correctness
  requirement, not a style suggestion [1] §12.3 p. 128.
- **Sub-pixel correction is sticky.** Rendering one triangle with fbzColorPath bit 26
  set corrupts the next triangle's parameters unless they are all re-sent [1] §12.4
  p. 128.
- **clutData and dacData want opposite video states.** CLUT writes are ignored unless
  the video unit is *running* (fbiInit1 bit 8 = 0) [1] §12.5 p. 129; DAC accesses want
  the memory bus idle, achieved by video reset or vertical sync [1] §5.68 p. 75.
- **Texture memory is write-only** and reads return undefined data [1] §10 p. 116; a
  texture cannot span memory banks; texbaseaddr may legitimately wrap below zero
  [1] §10 pp. 116–117.
- **The LFB has a fixed 1024-pixel stride** whatever the mode, off-screen access without
  clipping is undefined, and LFB reads block until the whole pipe drains [1] §9
  pp. 114–115.
- **Triple buffering has three preconditions**: swap sync on, interval zero, and
  swapbufferCMD bit 9 clear [1] §5.25 p. 59.
- **The user-interrupt tag is last-writer-wins.** Unstalled USERINTERRUPTs overwrite
  each other's tags; only the stalling form is race-free, and with intrCtrl bit 5 clear
  the command is dropped entirely [1] §5.30 p. 61.
- **Mac traps**: the same-named Glide fragment can be shadowed by an older
  Voodoo1-era library with no version tie-break (the driver Read Me and the shipped
  file names both show the fragment/file mismatch: file "3dfx GlideLib2.x", fragment
  `3DfxGlideLib2.x` [8]); the card is fullscreen-only and windowed-OpenGL games fail;
  Mac OS 9 stability issues are acknowledged by 3dfx; blit-style double buffering
  (Rainbow Six) shows cursor trails against real page flipping [8].
- **The Mac stack never touches the interrupt pin.** The card's INTA# is fully specified
  ([1] §6.13, §5.2), yet nothing in the shipped Mac software stack arms intrCtrl — the
  drivers drive 3D entirely by polling the status register [8] (*observed* in the
  symbol strings of the whole driver set: no interrupt-service entry point appears
  outside the generic Glide library code).

## 6. Open questions

1. **The Bruce (TMU) specification is missing.** trexInit0/trexInit1 are the TMU's
   bring-up registers and the Voodoo2 spec says only "FIXME. See Bruce spec." [1] §5.90
   p. 85; the referenced document is not in this evidence set. The shipped drivers'
   values (SST_TREX0INIT0_DEFAULT etc. [6]) exist but are untied to any register
   description.
2. **The internal RAMDAC/PLL/encoder registers.** The spec asks "FIXME – what are
   registers for internal RAMDAC, PLLs, and NTSC/ENCODER??" [1] §5.68 p. 75 — the
   ATT/TI/ICS register maps are known only from the drivers' code and INI files [6] [7],
   not from a vendor datasheet. Which DAC the Power3D II and Game Wizard actually carry
   is not documented by either card page [10] [11].
3. **FIFO depth: 128 or 64?** §3.3 states a 128-entry asynchronous FIFO [1] p. 15; the
   status register counts 64 entries (0x3F = empty) [1] §5.1 p. 29. Whether these are
   two stages of one pipe or a spec defect is unresolved.
4. **maxRgbDelta** — the video-filter threshold register — is printed as "FIXME" in the
   specification [1] §5.48 p. 66; only the bit field survives.
5. **fbiInit7 bits 31:28** are not described anywhere in the register's own table
   [1] §5.59 p. 73.
6. **The Mac cards' DAC and clock details**: which RAMDAC part, and what graphics/video
   clocks the Mac driver actually programs for the Power3D II and Game Wizard (the
   hardware-museum page gives 90 MHz for the Power3D II [10]; the driver's default clock
   table for the Mac is not directly evidenced — the SSTV2_GRXCLK override exists but no
   shipping value is recorded).
7. **CMDFIFO on the Mac**: whether any Mac title or the Mac Glide library itself ever
   enables the CMDFIFO transport, or always drives registers directly through the FIFO
   (§3.1). The library contains the machinery; no capture of a live session is in the
   evidence set.
8. **The 2D BitBLT engine's Mac usage** — whether any shipped Mac software drives the
   blitter (§3.10), or whether all Mac 2D work goes through the LFB.
9. **Mac endianness split**: which of the hardware swizzle paths (register bit 20, LFB
   bits 11/12, tLOD bits 25/26) the Mac driver enables per color format, and where
   software conversion takes over — the API exports both hardware and software paths
   (§4.7), but the per-format choices are not recorded here.
10. **The iMac variants**: the Read Me excludes iMac cards ("iMac cards are not
    currently supported" [8]); what those cards are (a mezzanine-board form factor is
    implied by the driver's own wording) is not documented by any page in the evidence
    set.
11. **Power3D II board details beyond the chipset data**: memory part count, strap
    settings (fb_addr_a/b values, hence the card's actual class code and BAR default),
    and the pass-through circuit — no schematic of either Mac card is available.
12. **The driver betas' versions and dates beyond the 3dfxBIOS list** [9]: the exact
    Glide library builds (the archives' `vers`/`cfrg` resources carry build numbers that
    are not transcribed here), and whether beta 2/beta 4 differ from beta 5 in more
    than the Read Me's listed fixes.
13. **Interrupt routing on a real Mac**: the card asserts INTA#; which open PIC / Grand
    Central line a TNT machine maps it to, and whether the Mac library ever arms
    intrCtrl at all (§3.7) — the driver's polling style suggests not, but absence of
    strings is weak evidence.

## References

1. 3Dfx Interactive, Inc., *Voodoo2 Graphics: High Performance Graphics Engine for 3D
   Game Acceleration*, specification revision 1.16, December 1, 1999 — §1 "General
   Description" pp. 7–8 (feature lists, PCI 2.1/66 MHz, bi-endian, memory
   configurations); §2 "Performance" p. 9 (75 MHz rates); §3 "Architectural and
   Functional Overview" pp. 11–19 (Chuck/Bruce division p. 11, DT/TT bus and the
   1–3-Bruce configuration charts pp. 11–13, SLI p. 13, slave-only/FIFO/memory
   architecture p. 15); §4 "Voodoo2 Graphics Address Space" p. 20; §5 "Memory Mapped
   Register Set" pp. 21–99 (address field decode pp. 21–22, register table pp. 22–29,
   status p. 29, intrCtrl p. 30, triangle parameters pp. 31–36, fbzColorPath pp. 36–40,
   fogMode pp. 41–42, alphaMode pp. 43–46, fbzMode pp. 46–50, lfbMode pp. 50–57,
   commands and counters pp. 58–64, fbiInit0–7 pp. 67–73, CMDFIFO registers pp. 74–75,
   clutData/dacData p. 75, setup engine pp. 76–79, TMU registers pp. 79–87, BitBLT
   registers pp. 87–99); §6 "PCI Configuration Register Set" pp. 102–107; §7 "3D
   Command Descriptions" pp. 108–109; §8 "2D Command Descriptions" pp. 110–114; §9
   "Linear Frame Buffer Access" pp. 114–115; §10 "Texture Memory Access" pp. 116–119;
   §11 "CMDFIFO Operation" pp. 120–127; §12 "Programming Caveats" pp. 128–129; §13
   "Video Timing" pp. 130–131; §14 "Revision History" p. 132.
2. 3Dfx Interactive, Inc., *Voodoo Graphics (SST-1) Specification*, revision 1.61,
   December 1999 — §6.2 "Device_ID Register" p. 102 (the "Default is 0x1" text the
   Voodoo2 document copies); the SST-1 register architecture and linear-framebuffer/
   texture-memory chapters that the Voodoo2 specification inherits and cites.
3. 3Dfx Interactive, Inc., *Glide Programming Guide*, version 2.4, July 1997 —
   Chapter 3 (startup sequence, `grSstQueryBoards`/`grGlideInit`/`grSstWinOpen`, the
   display buffer and aux-buffer rules, buffer layout figure "Logical Layout of the
   Linear Frame Buffer", masking, `grBufferSwap`/`grBufferNumPending`, vertical
   retrace); Chapter 10 (texture memory management); Chapter 11 (linear frame buffer
   access).
4. 3Dfx Interactive, Inc., *Glide Reference Manual*, version 2.4, July 1997 —
   `grSstControlMode` p. 67 (GR_CONTROL_ACTIVATE/DEACTIVATE and the VGA/Voodoo display
   switch); `grSstWinOpen` p. 82; the LFB entry points.
5. Apple Computer, Inc., *Designing PCI Cards and Drivers for Power Macintosh
   Computers*, revised edition (March 26, 1999) — §"Open Firmware FCode Options",
   "No Open Firmware Support" p. 88: enumeration of a card with no FCode or no
   expansion ROM (recognized, address space allocated, no driver loaded, no name
   property).
6. 3Dfx Interactive, Inc., Glide run-time source code release (Glide 2.x/3.x, open
   source) — the CVG initialization module `glide2x/cvg/init/`: `sst1init.c`
   (`sst1InitMapBoard`, `sst1InitRegisters`, `sst1InitCheckBoard`, PCI device search
   with vendorID 0x121A / deviceID 0x0002, the reset ordering and its comments),
   `sst1init.h` (SST_FBIINIT0–7_DEFAULT, SST_TREX0INIT0/1_DEFAULT, DAC register
   defaults MIR/DIR 0x84/0x09 AT&T and 0x97/0x09 TI, ICS PLL power-on values,
   snoop default), `dac.c` (`sst1InitDacDetect` and the ICS/ATT/TI/INI probes),
   `voodoo2.ini` (the `[Voodoo2]`/`[DAC]` configuration file format with the
   dacWr/dacRd command sequences).
7. Linux kernel `sstfb` framebuffer driver (`drivers/video/sstfb.c`, `sstfb.h`,
   `Documentation/fb/sstfb.rst`) and `include/linux/pci_ids.h` — the independent
   second driver: Voodoo2-specific limits and tiling in `sstfb_check_var`/
   `sstfb_set_par`, DAC detection comments, the `__BIG_ENDIAN` swizzle branch, and the
   PCI ID table (0x121A/0x0001 Voodoo, 0x0002 Voodoo2, 0x0003 Banshee, 0x0005
   Voodoo3, 0x0009 Voodoo5).
8. 3Dfx Interactive, Inc., *Voodoo2 driver for Macintosh*, version 1.0 beta 5
   (archive "VOODOO2_DRV_1.0B5.sit", opened with the in-tree archive library): the
   "Voodoo2 Read Me" and "OpenGL Notes" texts quoted in §4.5, and the four Extensions
   files `3dfx GlideLib2.x`, `3Dfx GlideLib3.x`, `3dfx Rave`, `3dfx OpenGL Renderer`,
   whose string tables carry the NameRegistryLib/PCILib/ExpMgrConfig* imports, the
   `sst1Init*`/`pciFindCardMulti`/`.SstSetupMac` symbols, the RAVE engine name
   "3Dfx SST-1", the grLfbLock/grLfbUnlock/grLfbWriteRegion/grLfbWriteColorSwizzle/
   guEndianSwap* exports, and the SSTV2_* configuration-variable list of §4.8.
9. 3dfxBIOS, "VoodooMAC" Macintosh driver list (saved reference page) — the Voodoo2
   Mac driver releases and dates: 1.0 beta 2 (1999-08-10), 1.0 beta 4 (1999-12-06),
   1.0 beta 5 (2000-01-25); the Voodoo Graphics driver of 1997-08-27.
10. Hardware Museum (hw-museum.cz), "Techworks Power 3D II" card page — SSTV2 chipset
    data (350 nm, 4 M transistors, 90 MHz), 12 MB EDO memory on a 192-bit bus,
    TechWorks manufacture, PCI interface, DB-15-class analog output.
11. VGA Legacy MKIII (vgamuseum.com), "3Dfx Voodoo 2" card page — the Voodoo2 card
    family summary (4 MB framebuffer, 4/8 MB texture memory, 800×600, 1024×768 with
    SLI), and "Micro Conversions Game Wizard is version for Apple computers using
    DB15 video output".
12. Wikipedia, "Voodoo2" article (saved reference page) — introduction February 1998;
    90 MHz chipset clock with 100 MHz EDO DRAM; the 8 MB / 12 MB configurations; the
    single-cycle dual-texturing claim; SLI doubling the framebuffer and raising the
    maximum resolution to 1024×768.
