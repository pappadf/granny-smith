# The Apple QuickDraw 3D Accelerator Card ("Gotham")

**Contents:**

1. [Overview](#1-overview) — what the card is, the board and its silicon, the White Magic lineage, host machines,
   the evidence base and its limits
2. [Register file](#2-register-file) — what is documented and what is not; the object-data token interface; the
   on-chip pixel state; the texture memory interface and its map; control state; clocks
3. [Behaviour](#3-behaviour) — host/card division of labor, the frame pipeline, hidden-surface removal and
   multi-pass overflow, transparency, CSG, the texture path, scanout, multiple cards, interrupts, heat
4. [Programming model](#4-programming-model) — QuickDraw 3D and the integration point, the driver extensions,
   driver-side software work, texture management, observed run-time behaviour and performance
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 What the card is

The **Apple QuickDraw 3D Accelerator Card** — card codename **Gotham**, a late member of the project line Apple
called **White Magic** — is a 3D-only rendering coprocessor on a PCI card: it carries no display output of its
own, drives no connector, and holds no framebuffer. It renders shaded, textured, transparent and
constructive-solid-geometry (CSG) geometry into *whatever framebuffer the host already owns*, including the
built-in video of the machine it is installed in; "a single card accelerates 3D rendering to all frame buffers
in the system" [1] §"Scalability". Apple positioned it as "workstation-class 3D acceleration" for any
Power Macintosh or Performa with a PCI slot, at a street price of roughly 400 US dollars in November 1995 [4].

Apple's published numbers for the card [1] §"Features" [2]:

| Characteristic | Value |
|---|---|
| Peak rendering speedup vs. software | "up to 12 times" QuickDraw 3D rendering |
| Textured fill rate | 10 million trilinearly filtered, mipmapped texture pixels/second |
| Triangle throughput | up to 120,000 triangles/second |
| Geometry features | Gouraud shading, texture mapping, transparency, CSG |
| Fixed rendering features | trilinear filtering, mipmapping, diffuse and specular lighting always enabled |
| Texture store | "high-performance SRAM memory" for up to 12 texture maps per open window |
| Output | alpha channel out, "fast and easy to transfer images between applications" |
| Visibility model | hardware-accelerated per-pixel Z sorting; transparent surfaces and CSG correct "regardless of submission order or interpenetration" |
| Scalability | second card doubles hardware rendering performance |
| Framebuffer | none on card |
| Board size | 6.88 inches [1]; "7-inch" [2] |

The card is unusual among 3D accelerators in *when* it intervenes in the host software stack: it does **not**
plug into QuickDraw 3D RAVE, the low-level rasterization interface Apple later published for 3D chip vendors.
It integrates one level higher, inside the QuickDraw 3D renderer layer itself, so only applications that go
through QuickDraw 3D's high-level (interactive-renderer) interface can use it — every RAVE-specific title of
the later Mac game catalog is invisible to the card (§4.1, §4.5). This is the single most consequential fact
about the card's place in a system, and it is established by observation [15], not by any Apple document.

### 1.2 The board and its silicon

The card is built around **two custom Apple ASICs** — a rendering engine "based on state-of-the-art O.5µ
technology" (0.5 µm) plus a second custom part — with 512 KB of high-speed SRAM texture memory and a 128 KB SRAM
cache, no framebuffer, and no video connectors [1] [3]. Contemporary press reporting from its launch window
describes the same population: "a rendering engine and two special ASICs as well as 128 KB static RAM cache
and 512 KB static main memory" [4]. The physical record of the boards (front/rear photographs, part and
assembly numbers on the silkscreen) identifies the card as Apple part **M4142**, assembly **820-0739** and a
-**A** revision, with earliest production-week markings around week 35 of 1995 (*observed* on the board
photographs of the research record [15]; the part numbers are not corroborated by an Apple document).

What each silicon block is:

- **The rasterizer ASIC** — the modified-scanline rendering engine of §3, clocked at 40 MHz [5]. On final
  production silicon it reportedly carries the Apple part-style marking **343S1167** (*community-reported*
  from a teardown; not confirmed against an Apple drawing — a photographic confirmation is outstanding,
  [§6](#6-open-questions)) [15]. The logic is a 3.3 V design that community overclocking experiments ran
  reliably at 44 MHz and, with a faster oscillator, at up to 54 MHz once the cache SRAM is replaced with
  12 ns parts — evidence both that the core has headroom and that the shipped 40 MHz oscillator is the
  deliberate, conservative choice (*observed*) [15].
- **The second custom ASIC / companion chip** — one TI-manufactured part sits next to the rasterizer; its
  exact identity and division of labor are **not established**. The candidates are a PCI bus bridge or a
  command-processing front end; the architecture papers split the system into a rendering component plus a
  bus-attachment/object-fetch function, but no public document names the part (*inferred from board
  photographs and the architecture's division of labor* [5] [8] [15]; see [§6](#6-open-questions)).
- **Texture SRAM** — 512 KB organized as two banks of 131072×16-bit words holding even and odd texture pixels
  (§2.4) [9] [15].
- **Object cache SRAM** — 128 KB holding the Z-sorted list of polygons (the active-list/object stream the
  rasterizer consumes, §3.1) [5] [15].

The card dissipates enough heat to matter: in a packed Power Macintosh 7500 both observed cards ran hot
enough to require added coolers, and in a 9500 one of two installed cards ran hot while the other stayed
merely warm (*observed*) [15]. Overclocking notes consistently name heat as the first limit, before SRAM
speed [15].

### 1.3 The White Magic lineage

The card is the shipped product of an Apple research line that ran from the late 1980s inside Apple's
advanced research group, through three documented hardware generations. The published papers and the patent
family describe each generation; the naming is *White Magic* for the project, *Gotham* for the card
(*community-reported, consistent across the research record* [15]):

| Generation | Date | Interface | Architecture | Performance | Textures |
|---|---|---|---|---|---|
| White Magic prototype | 1992 | 68040 Processor Direct card in a Quadra [6] | two 0.8 µm chips at 40 MHz; scanline Z-buffer; 80 bits/pixel on-chip (32 Z + 8 shadow + 10 bits each aRGB); 640-pixel scanline [6] | 40M alpha-blended Z-buffered pixels/s; 880K triangles/s in a 4-wide parallel configuration [6] | none [6] [15] |
| Gotham | 1994 (prototype), late 1995 (product) | PCI card for the Power Macintosh [1] [5] | single rendering ASIC at 40 MHz; 16-pixel scanline segments; four-layer Z-ARGB state on-chip at 480 bits/pixel; CSG evaluator; deferred trilinear texturing (§2, §3) [5] | 20M pixels/s untextured, 10M pixels/s trilinearly textured, 120K triangles/s [1] [5] | trilinear mipmapped, 256×256 max [1] [9] [15] |
| Unshipped successor | 1996/97 designs, never productized | PCI or AGP; rasterizer + I/O-interface ASIC pair | 16×32-pixel tiles; modified A-buffer with an 8-bit staggered subpixel mask; 25-bit floating-point depth; on-chip texel caches; up to 4 rasterizers per I/O ASIC [7] [14] | 100M pixels/s at 100 MHz; 2M texture-mapped triangles/s; hardware antialiasing at a 30–40% penalty [7] | up to 2048 texels/side, 16 or 32 bits/texel [7] |

The three generations share one architectural idea: keep the per-pixel rendering state small enough to live
on-chip — a scanline in 1992, a 16-pixel segment in 1994, a 16×32 tile in 1997 — so that the depth buffer and
compositing state never need off-chip bandwidth, and render front-to-back so that transparency and CSG fall
out of the same per-pixel Z machinery [5] [6] [7]. The 1997 design study states the continuity explicitly: it
"describes a low cost hardware accelerator for rendering 3D graphics with antialiasing... based on a previous
architecture" — the 1994 system — and the two papers share authors [5] [7].

### 1.4 Host machines

The card works in "any Power Macintosh or Macintosh Performa computer with a PCI slot", with a PowerPC
processor, 16 MB of RAM, and System 7.5.2 with System Enabler 1.1 or later; Apple's examples are the Power
Macintosh 7200, 7500, 8500 and 9500 [1]. The PCI environment itself — configuration cycles, memory and I/O
windows, the big-endian host to little-endian bus seam, the Open Firmware startup contract — is
[pci.md](../pci.md)'s (§2, §4).

Observation places the card in two of the TNT-family machines ([tnt.md](../../../machines/tnt/tnt.md) §1.2):
the **Power Macintosh 7500** ([pm7500.md](../../../machines/tnt/pm7500.md) §1.1), where two cards were run
together, and the **Power Macintosh 9500** ([pm9500.md](../../../machines/tnt/pm9500.md) §1.1) with its six
PCI slots — three on the processor bus and three behind the PCI-PCI bridge
([bandit.md](../../../machines/tnt/bandit.md) §1.4) [15]. The framebuffers it accelerates in those machines
are ordinary display hardware: the TNT platform's built-in video ([tnt.md](../../../machines/tnt/tnt.md)
§2.4), the 9500's bundled Apple mach64 GX display card ([mach64.md](mach64.md) §1.2), or a third-party PCI
board such as the TwinTurbo-based 9600 Graphics Accelerator ([twinturbo.md](twinturbo.md) §1.3) — the card
writes to all of them alike, which is its point [1]. Two behavioural facts of the platform pair matter to
this card:

- The card works in any of the six slots, but renders about **10–15% faster in the bus segment where the
  framebuffer lives** (*observed* on a 9500) [15] — the scanout path (§3.7) writes the finished pixels over
  PCI to the host framebuffer, so a bridge hop on either end costs bandwidth
  ([pci.md](../pci.md) §3.3).
- A single card reached 40–50 fps and a two-card setup 70–85 fps in the Gerbils! test application on a
  9500 upgraded with a 375 MHz PowerPC G3 (*observed*) [15].

### 1.5 Evidence base and its limits

Every hardware fact on this page is derived from primary evidence: Apple's own product documentation — the
card's Read Me as archived in the Tech Info Library [1], the October 1995 sales-internal product sheet [2]
and the specification table [3]; the three SIGGRAPH papers by the chip's designers, which describe the 1992
prototype, the 1994 Gotham architecture and the 1997 successor in detail [5] [6] [7]; the Apple patent family
covering the scanline pipeline and its token interface, the texture memory organization, the object cache,
and the multi-layer Z-buffer [8] [9] [10] [11] [12] [13] [14]; and a sustained public hardware investigation
of the physical card [15] — oscilloscope probing of the rasterizer's SRAM interface, overclocking and
dual-card experiments, application compatibility testing, and benchmark measurement on Power Macintosh 7500
and 9500 hardware, plus the board photographs and the measured benchmark document. The QuickDraw 3D API
context comes from Apple's own developer material [16] and contemporary developer-press documentation [17].

The limits are unusual for a card page in this tree, and they shape the whole page:

- **No public expansion-ROM dump exists, and the card's PCI vendor/device IDs are not publicly documented.**
  Every fact about the card's host-visible register surface — the BAR set, the DMA descriptor formats, the
  interrupt registers, the FCode startup — is therefore missing here and lives in
  [§6](#6-open-questions) instead of §2.
- **The silicon itself is documented only at the architecture level.** The papers and patents give the data
  path, the algorithms and the internal state formats in genuine detail (§2.2–§2.6), but they are descriptions
  of the *design*, published as papers and patent text — a claim-by-claim confirmation against the shipped
  silicon is only possible where the hardware investigation touched the same point (the texture memory being
  the lucky case: the measured pinout matches the patent's preferred embodiment, §2.4). Claims that rest on
  papers alone are marked *inferred - unverified* where they concern the shipped card rather than the
  published design.
- What this page deliberately does **not** do is pad. Where the evidence runs out — the companion chip, the
  wire protocol deltas, the multi-card partition mechanism — the section says so and hands the question to
  [§6](#6-open-questions).

The re-implementation bar for this card is therefore stated honestly: the *rendering architecture* below is
documented to the register-and-token grade; the *host interface* is not documented at all, and a working
emulation of the card would additionally have to model the QuickDraw 3D integration layer that is its only
driver surface (§4.1).

## 2. Register file

### 2.1 What is documented and what is not

The card has no public register reference. There is no datasheet for the rasterizer ASIC, no expansion-ROM
image in the public record, and no published PCI identity: the vendor/device ID pair that
[pci.md](../pci.md) §2.5 makes the first fact of any PCI device is, for this card, unknown (§2.2 of that page
describes the standard header; the card's own values for it are not established). The register-grade material
that *does* exist is the object-data interface the rendering pipeline consumes (§2.2), the on-chip pixel state
(§2.3), and the texture memory interface and its address organization (§2.4–§2.5), each documented by the
designers in patent text [8] [9] and, for the texture memory, independently confirmed on the shipped hardware
by pin-level measurement [15]. The control state the driver loads before a frame (§2.6) completes the set.

Two register files deliberately do **not** appear here: the host-visible I/O surface (BARs, DMA control,
interrupt enable/status) and the companion chip's programming interface. Neither is in the public evidence;
see [§6](#6-open-questions).

### 2.2 The object-data interface: token formats

The rendering pipeline is fed a stream of **tokens** — fixed-format words that every processing stage
parses. The token interface is documented in the scanline-rendering patent's preferred embodiment [8], whose
pipeline (a vertical-interpolation stage feeding a Z-buffer stage feeding a compositing/scanout stage, with
the last two sharing one IC design) is the two-chip 1992 prototype's arrangement [6] carried forward; the
1994 Gotham design integrates the equivalent functions in one ASIC [5]. The token set therefore describes
the family's object language; the shipped card's exact wire encoding is a paper-level fact
(*inferred - unverified on the shipped card*).

Every token starts with the same two fields: a 1-bit **PSetup** flag (true for set-up tokens generated at
span-setup time, false for pixel and control tokens) and a 4-bit **TokenID** [8]. The pipeline decodes
hard-wired bit positions — the format *is* the register file of the data path [8].

**Pixel Interpolation Token** (TokenID 1) — the per-pixel work record that drives horizontal interpolation,
Z test and compositing [8]:

| Field | Width | Meaning |
|---|---|---|
| PSetup | 1 | false — not a set-up token |
| TokenID | 4 | 1 |
| X | 11 | pixel position in the current scanline segment |
| W | 12 | interpolation weight |
| A, R, G, B | 10 each | diffuse/shaded colour, 10 bits per component (40 bits total) |
| ForceAdditive | 1 | 1 = this interpolation composites in additive mode |
| Unused | 8 | reserved; must be zero |

The **null token** (PSetup false, TokenID 0) is a pixel interpolation token whose processing was terminated —
typically because Stage 2 found the pixel behind another object or in shadow; it flows down the pipeline as a
place-holder [8]. The **pixel overlay token** (TokenID 2) is the same layout at 10 bits per colour component
with a 2-bit W, and lets the control processor assign pixel values directly (titling and similar effects) [8].

**Set-up tokens** (PSetup true) carry the span endpoint parameters produced by vertical interpolation [8]:

| Token | TokenID | Payload (beyond PSetup/TokenID) |
|---|---|---|
| Z Set-up | — | Z0 (32 bits), a Diffuse bit (lighting calculations enabled), a Front bit (front-facing shadow plane), Z1 (32 bits) |
| Diffuse RGB Set-up | 0 | left-endpoint diffuse colour ×Kd: Ad0, Rd0, Gd0, Bd0 (9 bits each); right-endpoint Ad, Rd, Gd, Bd (9 bits each) |
| Specular RGB Set-up | 3 | left: Ns0 specular power (9 bits), Rs0, Gs0, Bs0 (9 bits each); right: Ns, Rs, Gs, Bs |
| Normal Set-up | 4 | left normal Nx0, Ny0, Nz0 (12 bits each); right normal Nx, Ny, Nz |

The 32-bit Z endpoint values define the depth of a span's two ends; depth is interpolated with W against them
(§3.3). The Z Set-up token's Front and Diffuse bits feed the shadow-plane machinery of the 1992 design
[6] [8].

**Control tokens** (TokenID 0xF, an 8-bit OpCode) manage the pipeline and the scanout DMA [8]:

| OpCode | Token | Function |
|---|---|---|
| 1 | Load Scanline DMA Write Register | writes the clear/constant register of a target stage (RGB and Z target bits; 40-bit Write Value) |
| 2 | Scanline DMA setup/start | Start (11 bits), Length (11 bits) in pixels, ScanoutEnable, ScanoutMode (1 = round the 40-bit pixel to 32 bits on output), WriteEnable — arms and starts the scanline DMA |
| 3 | Wait for Scanline DMA Completion | stalls (stallout asserted) until the back buffer finishes scanning out or clearing, so buffers are not swapped mid-scanout |
| 4 | Swap Buffers | swaps the double-buffered scanline buffers of the targeted stage |
| 5 | Global Mode Setup | the frame's mode bits (§2.6) |
| 6 | Jam Data | passes a 40-bit Data word through the pipeline unprocessed |

Two structural facts of the preferred embodiment complete the picture [8]: each scanline buffer is **648
words × 40 bits** (a double-buffered pair per stage, sized for the prototype's 640-pixel scanline), and the
vertex parameters are held in **four 64×36-bit parameter RAMs** addressed by parameter number and stream
context, with a 4×36-bit crosspoint switch feeding the linear interpolators — four vertices readable
simultaneously, three of them fetched per triangle by the vertex sort.

### 2.3 The on-chip pixel state: the four-layer Z-ARGB buffer

The heart of the chip is the per-pixel sorting state, and its exact size is documented [5]. Each pixel's
layer slot holds:

| Component | Width | Content |
|---|---|---|
| Z | 24 bits | depth of the layer |
| ARGB | 32 bits | composited colour and alpha of the layer |
| state | 4 bits | overflow flag plus CSG operand state (§3.5) |

Four layers are stored per pixel, and the store is double buffered so one 16-pixel segment sorts while the
previous one composites: **(24 + 32 + 4) × (4 layers × 2 banks) = 480 bits per pixel**, 7680 bits for a whole
16-pixel segment — "well within the cost constraints of the system" [5]. The depth buffer, the compositing
image buffer and the CSG state are all inside this structure; there is **no off-chip Z buffer** anywhere in
the system [5] [7].

The Z Set-up token's 32-bit endpoint Z values (§2.2) and the 24-bit stored Z define different precisions in
different parts of the family's documents — the 1992 prototype stored 32-bit Z [6], the 1994 design stores
24-bit Z per layer [5], and the 1997 successor uses a 25-bit floating-point depth (19-bit mantissa, 6-bit
exponent) [7]. Which endpoint precision the shipped Gotham silicon interpolates is not separately
established (*inferred to be the 24-bit per-layer store of the 1994 design* [5]; see [§6](#6-open-questions)).

### 2.4 The texture memory interface

The card's texture store is the one subsystem where the shipped hardware and the patent text meet
pin-for-pin. The preferred embodiment of the texture-storage patent [9] and the measured board [15] agree:

- **Two banks of 131072 words** — even-numbered texture pixels in bank A, odd-numbered in bank B [9]. Two
  pixels are fetched simultaneously, one from each bank, so the eight texel fetches of a trilinear sample
  (§3.6) collapse to four even/odd pairs [9].
- **24 address lines serve two simultaneous pixels**: the patent's alignment scheme (§2.5) makes the two
  addresses of a vertically adjacent pair differ only in the bottom *n* bits (*n* = log2 of the page's row
  size, at most 8 for a 256×256 map), so the upper address bits are shared — `{A17:8} shared + {A7:1} × 2
  banks = 24 unique address signals`, against 34 for two independent addresses [9].
- The measurement of the production board's rasterizer found **17 address lines** — ten shared (A1–A5,
  A8–A12) and seven per-bank (A0, A6, A7, A13–A16, bonded separately to each bank) — plus per-bank chip
  enables, a shared output enable, and data pins consistent with a two-cycle 32-bit-wide read
  (*observed*; the bank assignment of every line was traced on the PCB) [15]. Ten shared + seven separate =
  exactly the 24 signals of the patent's scheme, if the seven per-bank lines are counted twice.
- **Data width is the one visible cost reduction.** The patent's preferred embodiment moves **36 pixel
  data signals — 18 bits per texel** [9]; the shipped card wires **16 bits per texel**, and the two pins
  nearest the data bus that the patent's 18-bit format would occupy are unconnected on the board — but
  show activity on an oscilloscope exactly when textures are loaded (*observed*) [15]. The silicon can
  produce 18 bits (a 6-6-6 colour format is within its capability); only 16 are bonded out, for a two-cycle
  32-bit read path of about 160 MB/s at 40 MHz rather than the 64-bit, 320 MB/s path of the unreduced
  design [15] [9] (*the 64-bit figure is a community reading of the patent's width; the shipped width is
  observed*).

### 2.5 The texture memory map

The texture address organization is a documented algorithm, not just a layout [9]. All MIP-map **pages of the
same resolution from all maps are stored contiguously** (against the conventional layout, which stores each
map's pages contiguously), which aligns every page to a multiple of its own row size squared and reduces
pixel addressing to shift-and-OR:

```
address(x, y, page, map) = (x << n) | y | offset(page, map)
```

with *n* = log2(RowSize) of the page being accessed, and the offset built as `offset = M + T`, where
`T = MapID << (2·(n − P))` for page index P, and M is a fixed per-page base stored in a small look-up table
indexed by P [9]. For two 256×256 maps the page offsets are:

| Page | Resolution | Map 0 offset | Map 1 offset |
|---|---|---|---|
| 0 | 256×256 | 0 | 65536 |
| 1 | 128×128 | 131072 | 147456 |
| 2 | 64×64 | 163840 | 167936 |
| 3 | 32×32 | 172032 | 173056 |
| 4 | 16×16 | 174080 | 174336 |
| 5 | 8×8 | 174592 | 174656 |
| 6 | 4×4 | 174720 | 174736 |
| 7 | 2×2 | 174752 | 174756 |
| 8 | 1×1 | 174760 | 174761 |

(from Table 2 of [9]; a 256×256 map has pages 0–8, so P ranges over nine values)

The addressing parameters as the preferred embodiment defines them [9]:

| Parameter | Format | Meaning |
|---|---|---|
| u, v | 20-bit signed floating point, 11 fraction bits, implied leading 1 | incoming texture coordinates |
| x, y | n-bit integers derived from u, v (8 bits at the largest page) | texel coordinates within the page |
| P | unsigned 12-bit floating point, 4 fraction bits | MIP page selector; integer parts P and P+1 blend in trilinear filtering |
| MapID | 4 bits | texture map identity — **at most 16 maps resident** |

The 4-bit MapID is the origin of the card's texture-count limits: the final card advertises up to 12 texture
maps per window [1] [2], and the observed hard ceiling of the driver/hardware combination is 16 resident
maps [15]. A pre-release "seed" board of spring 1995 supported three textures, the production board 3 or 12
(*community-reported* from the research record's hardware testing [15]; where the 3-to-12 difference lives —
silicon, driver or both — is open, [§6](#6-open-questions)).

Texels are stored as **16 bits, in a 5-6-5 RGB arrangement**; the 18-bit (6-6-6) capability of the silicon is
unbonded (§2.4) [15]. Alpha in textures could not be enabled by the test software — either the card forbids
alpha-bearing textures or the enabling path was not found (*observed, negative result* [15]; §3.6, §4.4).

### 2.6 Control state: global mode, CSG LUT, scanline DMA

Three pieces of frame-scoped control state are documented.

**Global Mode Setup** (control token OpCode 5) initializes the pipeline's rendering mode before the frame
[8]:

| Bit field | Meaning |
|---|---|
| DiffuseShade | enable the diffuse shading contribution |
| SpecularShade | enable the specular shading contribution |
| ShadowCount | enable shadow counting |
| ShadowTest | enable the shadow test |
| TransMode | 1 = additive transparency blending, 0 = blended |
| InvertShadow | 1 = in-shadow is visible, 0 = out-of-shadow is visible |
| Force2Visible | 1 = force the Z test to return "visible" |
| DisableZWrite | 1 = do not write the Z/shadow bits |

**The CSG evaluation table** — a 32-bit register the driver loads before rasterization of the frame,
holding F_csg as a 32-entry look-up table indexed by the concatenation of the five per-pixel operand state
variables A′–E′ (§3.5) [5]. This is the card's programmable Boolean evaluator: arbitrary CSG expressions of
up to five operands are encoded as a truth table by the driver and consumed 5 bits at a time by the
compositor.

**Scanline DMA state** — the Scanline DMA setup/start token (§2.2, OpCode 2) programs the scanout: an 11-bit
start address and 11-bit length in pixels, the scanout enable, the write enable, and the 40-to-32-bit
rounding mode [8]. The Wait and Swap tokens sequence it against the double-buffered scanline buffer [8].

### 2.7 Clocks and reset

The pipeline family runs **three asynchronous clock domains**: the *data clock*, synchronous to the object
source, which paces the input port; the *pipe clock*, which drives the rendering stages and "can be
increased to match chip technology without affecting the rest of the system"; and the *scanout clock*,
synchronous to the receiving framebuffer [8]. On the shipped card the core runs at **40 MHz** [5] [6] [15],
on a 3.3 V supply (*observed from the oscillator and regulator probing* [15]). No reset state, power-on
register values or initialization sequence is in the public evidence; the flow-control signals the preferred
embodiment exposes (a STALL input and an EMPTY output per input port, with a 16-word-deep input FIFO per
32-bit port and four selectable input streams by SRCID [8]) are the closest documented analogue.


## 3. Behaviour

### 3.1 Division of labor between host and card

The system "splits the rendering task between software and hardware" [5]: the host PowerPC CPU performs
transformation, clipping and shading, and also builds the Y-bucketed triangle lists that drive rasterization
[5]. The card consumes those lists, rasterizes, sorts, composites and writes finished pixels to the host
framebuffer. The object cache (the 128 KB SRAM, §1.2) exists precisely to keep the active-list stream close
to the rasterizer: the object-cache patent describes generating an active list, fetching its objects from
system memory into "a second memory having a faster access time than the first memory", and feeding the
rendering circuit from that faster store [10].

The practical consequence — visible in benchmarks — is that the card does **not** absorb the geometry cost:
host-side transform, lighting, clipping and bucket sort scale with the CPU, so a faster host CPU raises the
card's measured triangle rate in complex scenes (*observed*: object composition and the polygon-level sort
run in software; scene throughput tracks CPU speed [15]).

### 3.2 The frame pipeline: bucket sort and active lists

The card renders a modified scanline algorithm over **16-pixel segments** [5] [15]:

1. The host bucket-sorts every triangle by its first active scanline (Y buckets) [5].
2. Per scanline, the card's active-list sequencer adds newly active triangles and horizontally bucket-sorts
   the active triangles by first touched 16-pixel segment (X buckets); the array of horizontal bucket
   pointers lives in the off-chip SSRAM object cache [5].
3. Rendering proceeds segment by segment: within a segment, the object-scanline intersection module computes
   span endpoints, span interpolation clips them to the segment and interpolates parameters at 20
   million pixels/second, and the Z-sort module inserts each pixel layer into the on-chip state at 40
   million layers/second [5].
4. The composite sequencer drains the previous segment's layers while the next segment sorts (the on-chip
   state is double buffered, §2.3), and the finished segment is scanned out to the framebuffer [5].

Spans are generated at up to 2.5 million per second [5]. The segment loop repeats until the scanline ends,
the scanline loop until the frame ends. The algorithm has no screen-size dependency in its state — the 1994
design renders images "of up to 8K×8K resolution in a single pass" [5].

The 16-pixel-segment size is a deliberate point on a curve the designers state explicitly: a 4×4-pixel
partition would have better two-dimensional locality, but "the other advantages of rendering in scanline
order (mainly simplicity) outweighed any potential gains" [5]. The unshipped 1997 successor moved to 16×32
tiles [7].

### 3.3 Hidden-surface removal and the multi-pass overflow

Within a segment, every pixel can hold the **four closest layers** seen so far (§2.3). An incoming layer is
inserted at its depth position in the per-pixel list; layers hidden behind an opaque layer are dropped at
insert time. The measured insertion costs are documented for random data [5]:

| Layers already held | Average clocks to insert |
|---|---|
| 0 | 1 |
| 1 | 1 |
| 2 | 1.5 |
| 3 | 2 |

so a fully four-layered pixel sorts in about 5.5 clocks against 4 clocks to composite — the two rates the
design balances [5].

If a pixel needs a fifth visible layer, the segment records an **overflow**. Overflow does not abort: the
card composites the four retained layers into one, stores the result in the first layer with the backmost
depth, and the *same segment's objects are resubmitted*; the next pass captures the next-closest layers,
and passes repeat "until overflow does not occur, effectively compositing an unlimited number of layers from
front to back" [5]. The segmentation is what makes this affordable: overflow is tested per 16-pixel segment,
so only the segments that actually need more passes resubmit — in the paper's 13056-triangle test scene the
total rendered-triangle count falls from 26112 (two whole-image passes) to 14231 once per-segment overflow
and the opacity stop are both applied, a 45% reduction [5].

Two rules of the sort matter to image correctness:

- **Objects hidden behind an opaque layer never cause overflow** — the "don't overflow for hidden layers"
  optimization stops compositing once an opaque layer is reached, which is why the sort must run front to
  back [5].
- **Equal-depth layers are retained, not replaced** — the multiple-layer Z-buffer patent covers exactly this:
  how a Z-buffer keeps objects with equal Z-values rather than letting the later object destroy the earlier
  one [11]. In the 1997 successor the rule is stated operationally: equal-depth layers composite in arrival
  order, first-come-first-rendered [7]; the same is *inferred* for the shipped card, whose equal-depth
  handling is the same patent family's subject [11].

### 3.4 Transparency and front-to-back compositing

Transparency is not a special mode; it is what the multi-layer machinery does for a living. The card
implements an interpolated transparency model with a single transmission coefficient per layer [5]:

```
Ir = IrFront + ktFront · IrBack        (same for Ig, Ib)
kt  = ktFront · ktBack
```

with the front object's diffuse and ambient contributions premultiplied so the specular component does not
dim as transparency increases, and each component saturating at 255 to absorb premultiplication overflow
[5]. Compositing walks a pixel's layers front to back, blending each visible layer in; the Global Mode
Setup's TransMode bit selects blended versus additive behaviour per frame (§2.6) [8], and the per-pixel
ForceAdditive bit selects it per token (§2.2) [8].

The advertised consequence is the card's best-known property: "hardware-accelerated per-pixel Z sorting
provides precise rendering of transparent surfaces or CSG, regardless of submission order or
interpenetration" [1] [2] — correct transparency without the application sorting its transparent geometry.

### 3.5 CSG evaluation

Constructive Solid Geometry is evaluated *inside the compositing pass*, in Z order rather than in expression
order [5]. The rasterizer maintains one state bit per operand geometry per pixel — A′ through E′, "inside
this operand" — and updates the bits as the front-to-back walk crosses each operand's boundary. At every
crossing the hardware evaluates the Boolean CSG function F_csg (the driver-loaded 32-entry truth table,
§2.6) over the concatenated state bits: if F_csg changes, the crossing is a real boundary of the constructed
solid and the layer is rendered; otherwise the layer is discarded [5]. The whole evaluator is "only ~500
gates" for five operands; per-pixel cost is six state bits (A′–E′ plus the previous F_csg) times the 16
pixels of a segment — 96 bits, less than one composited ARGB layer [5].

The five-operand limit is a stated design choice for interactive modelling, not a silicon accident [5]. At
the QuickDraw 3D level it is the reason CSG "is available only when a QuickDraw 3D-compatible accelerator is
present" [17]: the software renderer had no Boolean evaluation, and the card's is the implementation the API
exposes.

### 3.6 The texture path

Texturing is **deferred**: a pixel layer is texture-mapped only after hidden-surface removal and CSG
evaluation have decided it is visible [5]. For each such layer the engine fetches the eight texels of a
trilinear sample — four from MIP page P and four from page P+1, in four even/odd bank pairs (§2.4) — and
interpolates them weighted by the fractional u and v [9] [5]. Texture mapping is bandwidth-limited to 10
million pixels/second against the untextured 20 million, and "because texture is applied after hidden
surface removal and CSG evaluation, rendering throughput is typically not degraded" — hidden layers never
touch the texture RAM [5].

Trilinear filtering and mipmapping are **always on**: "continually enabled" per Apple [1], and confirmed on
hardware — no quality setting changes the card's speed, because the filter cannot be switched off [15].
The observed filtering is **per-scanline only**: the eight-texel trilinear sample is taken along the scanline
dimension, and vertical versus horizontal texture detail renders differently — a horizontal wood grain and
its rotated twin do not look the same (*observed* [15]). Maximum resident texture size is **256×256**
texels (*observed, tested* [15]); each map consumes its full MIP pyramid (§2.5).

Alpha in textures could not be exercised: test software found no way to enable a 15+1 texture format, and
the architecture gives a reason — texturing happens at the end of the pipeline, after all colour and
transparency calculation, so per-texel alpha has no compositing machinery left to act on it. A one-bit
"black is transparent" texture behaviour was observed once, in what looked like a special overlay mode, and
is **not** established as a general feature (*observed, unconfirmed* [15]; [§6](#6-open-questions)).

### 3.7 Scanout to the host framebuffer

Finished pixels leave the card as writes to the host framebuffer — the card owns no display memory of its
own (§1.1). The compositing output runs at 20 million pixels/second [5]; the scanout side of the pipeline
family is clocked synchronous to the receiving framebuffer (§2.7) and its DMA is programmed through the
Scanline DMA tokens with an 11-bit start and length and a 40-to-32-bit rounding mode (§2.2) [8]: the card
composites in 40-bit pixels (10 bits per component, §2.2) and scans out 32-bit framebuffer pixels, rounding
on the way out.

At the system level the write traffic is real PCI traffic, and it is bursty in small units: the observed
scanout moves **16-pixel tokens, one token at a time**, to the framebuffer over PCI [15] (*observed*; the
token size matches the card's 16-pixel rendering segment, §3.2). This is the mechanism behind two observed
behaviours: a card renders 10–15% faster when it sits in the same bus segment as its framebuffer (§1.4), and
three cards cannot fully exploit a 33 MHz 32-bit PCI bus — "the 33MHz 32bit PCI-bus is not capable managing
the amount of small DMA transfers needed to fully use the potential" [15] ([pci.md](../pci.md) §3.3 gives
the bus's own bandwidth behaviour).

### 3.8 Multiple cards

The card is designed to scale by replication: "hardware rendering performance can be doubled simply by
installing a second card" [1] [2], with rasterization-intensive work (Gouraud shading, texture mapping,
transparency, CSG) named as the part that scales [2]. The parallel-scanline patent family shows the intended
mechanism: multiple scanline rendering devices driven from a shared active list, with the input bandwidth
raised by `(Npar − 1)/Havg` to keep every rasterizer fed [12], and the object-cache patent's active-list
generation is written for exactly that feeding arrangement [10]. In the 1997 successor the equivalent
structure is explicit — one I/O-interface ASIC driving up to four rasterizer ASICs whose outputs merge in a
framebuffer interface [7].

How two *cards* (not chipsets) divide a frame — alternate scanlines, alternating segments, or a partition
split — is **not established** by any document, and the observation that a second card yields about 80%,
not 100% [15], constrains but does not determine the answer ([§6](#6-open-questions)).

### 3.9 Interrupts

No interrupt register map is public. The design intent on record is minimal: the rendering pipeline is to
"interrupt the processor only when it has finished processing a frame", leaving the CPU free to work on the
next frame's geometry [7]. Within the pipeline family the closest documented sequencing is token-level: the
Wait for Scanline DMA Completion token back-pressures buffer swaps against scanout completion, and the Swap
Buffers token swaps them [8]. What the shipped card raises on the PCI interrupt line, and how
"Apple QD3D HW Driver" services it, is unestablished ([§6](#6-open-questions)).

### 3.10 Clocking, power and heat

The shipped card runs its core at 40 MHz (§2.7). The observed limits [15]:

| Configuration | Result |
|---|---|
| 40 MHz (stock oscillator) | normal operation; runs hot in a full enclosure |
| 44 MHz | stable |
| 45 MHz | freezes within about ten minutes |
| 54 MHz (replaced oscillator; 12 ns object-cache SRAM; added cooling) | stable, heat-limited — "a passive cooler is needed" |

The heat observation is consistent rather than anecdotal: the same cards that merely got warm in a 9500 ran
hot in a more crowded 7500, and the overclocking path names heat before SRAM speed as the binding limit
[15]. Apple's own specification quotes only ambient limits (10–40 °C operating) [3]; no thermal or power
figure for the card itself is published.


## 4. Programming model

### 4.1 QuickDraw 3D and where the card hooks in

QuickDraw 3D is a high-level, cross-platform 3D API with a scene graph and retained objects, a common file
format (3DMF) for exchanging models, and "an extensible, plug-in rendering architecture" with "transparent
access to graphics accelerators — no changes to your code are needed to take advantage of
[QuickDraw 3D]-compliant boards" [17]. Applications normally render through the **interactive renderer**
interface, which selects between the software renderer and hardware acceleration without application
involvement [15] [17].

The card integrates at that level — inside the QuickDraw 3D renderer stack — and **not** through QuickDraw
3D RAVE, the rasterization-level interface that Apple added afterwards for third-party 3D chip vendors [15].
RAVE is the interface of every later Mac 3D accelerator ([rage-128.md](rage-128.md) §1, [voodoo1.md](voodoo1.md) §1);
this card predates that split and never speaks it. The consequences observed across the card's whole
application record [15]:

- Only applications built on the high-level QuickDraw 3D APIs can reach the card; a RAVE game does not see
  it at all ("not recognized", or an immediate error).
- Features the hardware has that the API exposes selectively — CSG above all — appear only when the
  application submits Boolean operations through the QuickDraw 3D object model [15], consistent with the
  documented rule that CSG in QuickDraw 3D exists "only when a QuickDraw 3D-compatible accelerator is
  present" [17].
- The QuickDraw 3D stack itself changed quickly around the card (the 1995 beta, 1.0b1c5 of April 1995, had
  no accelerator interface at all; 1.0.3 shipped October 1995 with the card [15]), so the card's behaviour
  is inseparable from the specific QuickDraw 3D version driving it.

### 4.2 The driver software

Two files make the card work, both installed into Extensions and named by Apple's own Read Me [1]:

| File | Role |
|---|---|
| **Apple QD3D HW Driver** | the card's device driver — the piece that talks to the PCI hardware |
| **Apple QD3D HW Plug-In** | the QuickDraw 3D-side plug-in that routes the interactive renderer to the hardware |

The card is disabled by quitting QuickDraw 3D applications, moving *only the plug-in* out of Extensions and
restarting them — the driver may stay loaded; the plug-in is the integration point, exactly as §4.1 implies
[1]. The retail CD additionally carries the QuickDraw 3D 1.0.3 stack and sample content (Gerbils!, the
Cumulus database, SimpleText 1.3.1, Scrapbook 7.5.1, sample 3DMF models and textures) [1].

System requirements, as Apple states them [1]: a PowerPC-based computer with a PCI slot, 16 MB of RAM, Mac
OS 7.5.2 with System Enabler 1.1 or later — and a display set to **thousands or millions of colours**, the
only modes in which acceleration occurs ("QuickDraw 3D will run if you choose another display format, but
rendering will not be accelerated") [1]. The colour-depth rule follows from the hardware: the card composites
in 40-bit colour and scans out 32-bit pixels (§3.7), a conversion only direct-colour framebuffers can
accept. Apple also recommends turning Virtual Memory off on 16 MB systems to avoid crashes [1].

### 4.3 What the driver does in software

The hardware documentation assigns the host everything ahead of rasterization (§3.1), and the observation
record confirms the load is genuinely host-side [15]:

- transform, lighting and clipping of the geometry;
- the polygon-level composition and Z-ordering of the object stream;
- the Y bucket sort of §3.2's step 1;
- building and queuing the object lists in the card's object cache.

A faster CPU measurably raises the card's throughput in complex scenes (*observed* [15]); conversely the
card's own rasterization limits dominate flat, fill-rate-bound scenes.

### 4.4 Texture management

Texture residency is the card's tightest constraint and the driver manages it actively [15]:

- The texture store holds **512 KB** — at most 16 maps by the MapID field (§2.5), of which the product
  advertises 12 usable per window [1] [2] and the driver reportedly exposes fewer in practice
  (*community-reported* [15]).
- The maximum resident texture is **256×256**; QuickDraw 3D automatically rescales any larger texture down
  by software averaging **before** uploading it to the card (*observed* [15]).
- Upload converts to the card's **16-bit 5-6-5** texel format (§2.5) [15].
- The driver is **hard-wired for 512 KB**: a hardware modification that doubles the texture SRAM to 2 MB
  produces no software-visible change — texture capacity and behaviour stay identical (*observed* [15]).
- Per-texel alpha is not part of the texture path (§3.6); texture transparency is achieved by preparing the
  object as transparent and shading it accordingly, and nothing in the evidence shows the driver doing so
  [15].

### 4.5 Observed run-time behaviour

The card identifies itself through the QuickDraw 3D engine descriptor — the engine probe a QuickDraw 3D 1.6
SDK sample prints. The card's descriptor, probed on live hardware [15]:

| Field | Value |
|---|---|
| ASCIIName | "Apple PCI Hardware" |
| VendorID | 0 |
| EngineID | −1 |
| Revision | 1 |
| FastFeatures | Gouraud, Texture, TextureHQ, Blend, ZSorted |
| OptionalFeatures | DeepZ, Texture, TextureHQ, Blend, BlendAlpha, ZSorted, NoClear |

against a same-machine RAVE card's descriptor (a Formac/S3 ViRGE reporting VendorID 1716347235, EngineID 2,
FastFeatures Line/Gouraud/Texture/TextureHQ) — different fields, different interface, and the
"Apple PCI Hardware" engine never appears as a RAVE device [15]. The reported VendorID 0 and EngineID −1 are
placeholder-ish values whose meaning is not documented ([§6](#6-open-questions)).

Application behaviour, from the card's compatibility record on Power Macintosh 7500/8500-class machines
under Mac OS 8.6 and QuickDraw 3D 1.6 [15]:

- Works fully: **Havoc**, **Virtual Wings** 1.0.2, **Virtual Chess** (the last with slight render
  differences against the software renderer).
- Works with texture errors (missing textures, the 512 KB store overflowing): **Nanosaur**, **Weekend
  Warrior**, **Bugdom**.
- Not recognized or fails: the RAVE titles — Tomb Raider 1/2, VR Soccer, MechWarrior 2, NASCAR Racing,
  IndyCar Racing, HotCarts, Descent 1/2, Unreal; the Quake RAVE patch freezes with a type-10 error; Quake II
  is OpenGL-only. This is §4.1 made visible: a RAVE path has no way to reach the card.

### 4.6 Observed performance

Measured on a Power Macintosh 7500 with a 200 MHz PowerPC 604e under QuickDraw 3D 1.6 (Sneak3view synthetic
tests and the Gerbils! real-scene test) [15]:

| Test | Single card | Two cards | S3 ViRGE (86C325) |
|---|---|---|---|
| 25-px Gouraud triangles/s (320×240) | 141,484 | 169,029 | 158,831 |
| 50-px Gouraud triangles/s | 103,733 | 140,804 | 150,100 |
| 1000-px Gouraud triangles/s | 9,467 | 16,478 | 17,802 |
| 25-px textured triangles/s | 109,886 | 120,349 | 125,100 |
| 50-px textured triangles/s | 84,123 | 102,447 | 110,500 |
| 1000-px textured triangles/s | 9,022 | 15,008 | 7,424 |
| 1000-px textured triangles/s (640×480) | — | 18,070 | 6,485 |
| Gerbils! 640×480×16, fps | 31 | 55 | 15 |

The measured geometry rates sit at and above the 120,000 triangles/second specification for small triangles
and fall to about a tenth of it for screen-filling ones — consistent with a 20 million-pixel/s untextured and
10 million-pixel/s textured fill limit (§1.1, §3.6), and with the deferred-texturing design paying off on
large triangles: at 1000 textured pixels the card leaves the ViRGE (which textures before depth) behind by
2× [5] [15]. Real measured output works out to about 11 million pixels/s and 10 million texels/s against
the 20/10 specification figures — the specification's textured number is honest, and the untextured peak is
not reachable once the token-based scanout (§3.7) is in the path [15].

Scaling: two cards give roughly 1.2× to 1.8× depending on the test — about 80% of an ideal doubling [15] —
and the dual-card setup's real-scene performance lands in the region of a basic ATI Rage Pro or a first
generation 3Dfx Voodoo Graphics board, against which a single card is 2–4× slower in raw tests [15]
(*comparative figures measured by the research record; the other cards' own pages are*
[rage-128.md](rage-128.md) §1 *and* [voodoo1.md](voodoo1.md) §1). On a Power Macintosh 9500 with a
375 MHz G3 upgrade, Gerbils! runs at 40–50 fps on one card and 70–85 fps on two [15] (§1.4).

## 5. Quirks & errata

1. **Sixteen-pixel-wide bands on depth-tie overlap.** Where two surfaces' depths cannot be resolved
   accurately enough, the card draws a visible 16-pixel-wide pattern where textures and objects overlap
   (*observed* [15]) — the width is the rendering segment of §3.2, so the artifact is the per-segment sort
   boundary showing through. The family documents depth equality as an exact-match comparison ([7] for the
   successor; [11] for the equal-Z retention), which is the robust-but-strict choice the designers made in
   preference to a tolerance that "causes serious artifacts" [7].
2. **One-dimensional trilinear filtering.** The always-on trilinear filter samples along the scanline only,
   so identically textured geometry renders differently at different orientations (*observed* [15];
   §3.6). No Apple document mentions the asymmetry.
3. **Upscaling artifacts.** Magnified textures show noise because a screen pixel becomes larger than the
   16-pixel rendering token the filter works over (*observed* [15]).
4. **Missing textures under store pressure.** Applications that exceed the 512 KB texture store — Nanosaur,
   Weekend Warrior, Bugdom — render with missing textures rather than degrading to smaller maps
   (*observed* [15]); the driver rescales oversized maps down (§4.4) but has no fallback once the 12-map
   budget is spent.
5. **Hard-wired texture memory size.** The driver assumes 512 KB; doubling the physical SRAM changes
   nothing in software (*observed* [15]) — a hardware mod the shipped software cannot exploit.
6. **Sixteen-map residency ceiling.** The 4-bit MapID (§2.5) caps resident maps at 16 [9] regardless of
   free memory; the product's own limit is 12 per window [1].
7. **No texture alpha.** No tested path enables per-texel alpha; the architecture applies texture after all
   compositing (§3.6), so alpha-in-texture has nothing to composite against [15].
8. **Colour-depth gate.** Acceleration happens only with the display at thousands or millions of colours
   [1] — an 8-bit framebuffer silently falls back to software rendering.
9. **RAVE blindness.** The card never appears as a RAVE engine; RAVE-only applications cannot use it
   (§4.1, §4.5) [15].
10. **Runs hot.** Stock cards run hot enough to need airflow in a full enclosure, and the heat limits
    overclocking before the SRAM does (§3.10) [15].
11. **First-come order at equal depth.** Equal-depth layers render in arrival order
    (*inferred* for the shipped card from the family's equal-Z rule [11] and the successor's explicit
    first-come-first-rendered statement [7]) — a scene whose correctness depends on the order of
    coincident surfaces is submitting to a defined but unusual contract.
12. **The token stream's reserved bits are policed by convention only.** The object-data interface marks
    several fields "reserved; must be zero" [8]; nothing in the public evidence shows the hardware checking
    them, so a nonconforming stream's behaviour is undefined.


## 6. Open questions

1. **PCI identity.** The card's vendor ID, device ID, class code, revision and BAR set are not publicly
   documented, and no expansion-ROM image exists in the public record — the first table of any PCI card page
   ([pci.md](../pci.md) §2.5) cannot be written for this card. Whether the card even carries an expansion
   ROM with FCode, or is driven purely by the Mac OS extension, is unknown.
2. **The host-visible register surface.** BAR contents, DMA descriptor formats, the command mailbox, and the
   interrupt enable/status registers are all absent from the evidence; "Apple QD3D HW Driver" is the only
   known program for them and has not been analyzed in the public record.
3. **The companion chip.** The second, TI-manufactured ASIC's identity and division of labor — PCI bridge,
   command processor, or both — is not established; the architecture papers name the *functions* (bus
   attachment, object fetch) but no document maps them to the physical part.
4. **Token-format fidelity.** Whether the shipped card consumes the §2.2 token stream exactly as the patent's
   preferred embodiment defines it (field widths, TokenID assignments, control OpCodes) is unverified — no
   logic-analyzer capture of the object-data interface exists.
5. **Depth format.** 24-bit stored Z (1994 design) versus 32-bit Z endpoints (patent tokens) versus 25-bit
   floating point (1997 successor): the shipped card's working depth format and its tie behaviour are
   unconfirmed (§2.3, quirk 1).
6. **The multi-card partition mechanism.** How two cards divide a frame — alternating scanlines, alternating
   16-pixel segments, or a region split — is not documented; only the ~80% scaling and the PCI-bus ceiling
   of small DMA transfers are observed (§3.8).
7. **The 343S1167 marking.** The final-silicon part number is community-reported from a teardown and not
   confirmed photographically; neither ASIC carries a public Apple part number, and the 0.5 µm process node
   rests on the specification table's "state-of-the-art O.5µ technology" alone [3].
8. **Texture data width.** The patent's 36-signal (18-bit texel) interface against the 16 bits wired: the two
   unconnected pins with load-time activity are the presumed remainder, but the exact data bus width and the
   SRAM parts' speed grades on the production board are not recorded (§2.4).
9. **Alpha-in-texture.** Whether the hardware truly cannot composite per-texel alpha, or a 15+1 format
   exists behind an untested path, is open; the single observed "black is transparent" overlay-like mode is
   uncharacterized (§3.6).
10. **Seed-card versus production differences.** The spring-1995 pre-release board supported three textures
    against the production board's twelve, and the production run itself reports both "3 or 12" — where the
    limit lives (silicon revision, driver version, or available RAM) is unknown [15].

11. **The engine descriptor's odd values.** VendorID 0 and EngineID −1 in the QuickDraw 3D 1.6 probe are
    undocumented placeholders; what the plug-in actually reports through, and how the descriptor fields map
    to QuickDraw 3D's engine records, is unestablished (§4.5).
12. **CSG exposure.** Which QuickDraw 3D calls compile to the F_csg truth table, how expressions beyond
    five operands are decomposed or rejected, and whether any shipping application ever exercised the
    evaluator on this card, are all unrecorded (§3.5).
13. **The object cache format.** The 128 KB store's active-list entry size and layout (the 1992 prototype
    used 40 bytes per transformed, shaded triangle [6]) are not published for the shipped card, nor is the
    cache's replacement policy.
14. **Power and thermal figures.** No power draw, junction or supply-current figure exists; only the
    qualitative heat observations and Apple's ambient operating range (§3.10).
15. **The unshipped successor.** Whether the 1997 design (16×32 tiles, texel caches, 100 MHz) was intended
    as a Gotham replacement product, and how far silicon travelled before the project was cancelled, is
    documented only by its paper and patents [7] [14].

## References

1. Apple Computer, Inc., *Apple Tech Info Library article 18811, "QuickDraw 3D Accelerator Card: Read
   Me"* — the Read Me file installed with the card; created 24 October 1995, last modified 27 April 1998
   (§"Features of Your QuickDraw 3D Accelerator Card", §"Scalability", §"Troubleshooting 3D Acceleration",
   §"Turning Off 3D Acceleration", §"Using QuickDraw 3D", §"General Troubleshooting").
2. Apple Computer, Inc., *Macintosh Cards Update — Information About New Cards, October 1995* ("For Sales
   Personnel Only"), product sheet L01595A "QuickDraw 3D Accelerator Card" — positioning, key selling
   points, product description, competitive advantages and Q&A; companion data sheet L01593A referenced
   therein.
3. Apple Computer, Inc., *QuickDraw 3D Accelerator Card* technical specification — the specification
   table (128 KB SRAM cache; 512 KB high-speed SRAM texture memory; no frame buffer; two custom ASICs in
   0.5 µm technology; PCI 2.1; environmental limits), as preserved verbatim by the Higher Intellect
   vintage-computing wiki page "Apple QuickDraw 3D Accelerator Card".
4. *Computerwoche* (IDG), "3-D-Grafik fuer PCI-Power-Macs", 10 November 1995 — the launch-window press
   report: availability mid-November 1995, approximately 400 US dollars, "a rendering engine and two
   special ASICs", 128 KB SRAM cache, 512 KB SRAM main memory, up to 120,000 triangles per second.
5. M. Kelley, K. Gould, B. Pease, S. Winner and A. Yen, "Hardware Accelerated Rendering of CSG and
   Transparency", *Proceedings of SIGGRAPH '94* (ACM), pp. 177–184, July 1994, DOI 10.1145/192161.192184 —
   the Gotham architecture paper: the single-ASIC Power Macintosh rasterizer, 16-pixel segments, the
   four-layer 480-bit/pixel Z-ARGB store, the multi-pass overflow algorithm, the transparency model, the
   Z-ordered CSG evaluator with its 32-entry driver-loaded table, deferred texture mapping, the module
   diagram and the performance figures.
6. M. Kelley, S. Winner and K. Gould, "A Scalable Hardware Render Accelerator Using a Modified Scanline
   Algorithm", *Computer Graphics* 26(2) (*Proceedings of SIGGRAPH '92*), pp. 241–248, July 1992,
   DOI 10.1145/142920.134069 — the White Magic prototype: the two-chip 0.8 µm 40 MHz design, the on-chip
   scanline Z-buffer at 80 bits/pixel, direct-evaluation parallel scanlines, the 40-byte triangle record,
   and the measured performance analysis.
7. S. Winner, M. Kelley, B. Pease, W. Rivard and A. Yen, "Hardware Accelerated Rendering of Antialiasing
   Using a Modified A-buffer Algorithm", *Proceedings of SIGGRAPH '97* (ACM), 1997,
   DOI 10.1145/258734.258872 — the unshipped successor: 16×32 partitions, the 8-bit staggered subpixel
   mask, 25-bit floating-point depth, the two-layer HSR/composite buffers, the texel cache pair, the
   100 MHz/100 Mpixel/2 M-triangle figures, up to four rasterizers per I/O ASIC, the end-of-frame
   interrupt principle, and the equal-depth first-come-first-rendered rule.
8. US Patent 5,517,603, *Scanline rendering device for generating pixel values for displaying
   three-dimensional graphical images* (M. Kelley, S. Winner; Apple Computer), filed 19 December 1994 as a
   continuation of application 811,570 (filed 20 December 1991), granted 14 May 1996 — the pipeline and
   token interface: Charts A–L (pixel interpolation, pixel overlay, Z/diffuse/specular/normal set-up, and
   the control tokens with their Opcodes and field widths), the 648×40-bit scanline buffers, the
   4×64×36-bit vertex parameter RAMs, the three asynchronous clock domains, the input FIFOs and
   STALL/EMPTY flow control, and the tristatable parallel scanline buffers.
9. US Patent 5,606,650, *Method and apparatus for storage and retrieval of a texture map in a three
   dimensional computer graphics system* (M. Kelley, S. Winner et al.; Apple Computer), granted
   25 February 1997 — the texture memory: the two 131072-word even/odd banks, the 24-shared-address
   scheme, the 36 pixel data signals, the same-resolution-contiguous MIP page layout with its offset
   table, the shift-and-OR address equation, and the u/v/P/MapID parameter formats.
10. US Patent 5,592,601, *Method and apparatus for incremental acceleration of the rendering process
    utilizing multiple scanline rendering devices* (M. W. Kelley, S.-C. Yen; Apple Computer), granted
    7 January 1997 — the active-list generation and fast intermediate object store of the card's
    128 KB object cache.
11. US Patent 5,583,974, *Computer graphics system having high performance multiple layer Z-buffer*
    (S. L. Winner et al.; Apple Computer), granted 10 December 1996 — retaining objects with equal
    Z-values in a multi-layer Z-buffer.
12. US Patent 5,307,449, *Method and apparatus for simultaneously rendering multiple scanlines*
    (M. Kelley, S. Winner, K. Gould; Apple Computer), filed 20 December 1991, granted 26 April 1994 —
    the parallel-scanline scheme and its input-bandwidth scaling `(Npar − 1)/Havg`.
13. The Apple rendering-pipeline continuation family — US Patent 5,701,405, *Method and apparatus for
    directly evaluating a parameter interpolation function used in rendering images in a graphics system
    that uses screen partitioning* (granted 23 December 1997); US Patent 5,706,415, *Method and apparatus
    for distributed interpolation of pixel shading parameter values* (granted 6 January 1998); US Patent
    5,808,627, *Method and apparatus for increasing the speed of rendering of objects in a display system*
    (granted 15 September 1998); US Patent 5,920,687, *Z-buffer storage based on opacity and depth using
    pointers* (granted 13 July 1999) — the deferred, screen-partitioned interpolation and depth techniques
    of the family.
14. PCT application WO 96/19780 A1, *Three-dimensional graphics rendering system* (Apple Computer),
    published 1996 — the 134-page application for the successor-generation system.
15. The public hardware investigation of the QuickDraw 3D Accelerator Card, 2016–2025 — a sustained
    community research record comprising board photographs (card front/rear and the retail CD, from which
    the M4142 / 820-0739(-A) assembly numbers and production-week markings are read), oscilloscope and
    continuity tracing of the rasterizer ASIC's SRAM interface (the address/data/enable pin map of §2.4),
    overclocking experiments (§3.10), single- and dual-card benchmarking on Power Macintosh 7500/604e-200
    and 9500/G3-375 hardware under QuickDraw 3D 1.6 and Mac OS 8.6 (§4.5, §4.6, including the Sneak3view
    engine-descriptor probe and the comparative benchmark document against the Matrox Millennium II and S3
    ViRGE), application compatibility testing (§4.5), texture-format experiments with a modified texture
    test program, and the development-history reconstruction of §1.3 (the White Magic/Gotham/seed-card
    lineage, the 343S1167 marking report and the QD3D 1.0b1c5/1.0.3 software history).
16. Apple Computer, Inc., *QuickDraw 3D Preliminary User Interface Guidelines*, June 1995 — QuickDraw 3D's
   design context: the user-centred interaction model, the viewer/interactive-renderer concept and the
   3D-application user classes the card was to serve.
17. "Programming 3D with QuickDraw 3D", *MacTech Magazine* — the QuickDraw 3D programming model as
    documented for developers: the plug-in rendering architecture, transparent accelerator access
    ("no changes to your code are needed"), the interactive software renderer's performance envelope, and
    the rule that CSG support exists "only when a QuickDraw 3D-compatible accelerator is present".
