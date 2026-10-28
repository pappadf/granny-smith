# The Apple QuickDraw 3D Accelerator Card ("Gotham")

**Contents:**

1. [Overview](#1-overview) — what the card is, the board and its silicon, the White Magic lineage, host machines,
   the evidence base and its limits
2. [Register file](#2-register-file) — what is documented and what is not; the object-data token interface; the
   on-chip pixel state; the texture memory interface and its map; control state; clocks; the PCI identity, the
   host-visible registers, the packet port, the renderer registers, control programs, object records and
   pixel-output descriptors
3. [Behaviour](#3-behaviour) — host/card division of labor, the frame pipeline, hidden-surface removal and
   multi-pass overflow, transparency, CSG, the texture path, scanout, multiple cards, interrupts, heat, the
   frame protocol
4. [Programming model](#4-programming-model) — QuickDraw 3D and the integration point, the driver extensions,
   driver-side software work, texture management, observed run-time behaviour and performance, the RAVE engine
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

- **No register documentation was ever published.** The card's host-visible interface — PCI identity, the
  BAR, the packet port, the renderer registers, the control-program and object-record formats, the
  pixel-output descriptors and the interrupt protocol — is reconstructed instead from the PowerPC code of
  Apple's own driver and plug-in, whose debug builds keep their function names [18] (§2.8–§2.14, §3.11,
  §4.7). That establishes exactly what the driver writes and expects; what the card does with a value the
  driver never checks remains *inferred*.
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
documented to the register-and-token grade from the designers' papers and patents, and the *host interface*
to the grade of the shipped driver's code (§2.8–§2.14) — enough to run Apple's unmodified driver and
plug-in, with the pixel-exact rules the software cannot observe listed in [§6](#6-open-questions).

## 2. Register file

### 2.1 What is documented and what is not

The card has no public register reference: there is no datasheet for the rasterizer ASIC and no
expansion ROM on the card. Its PCI identity and host-visible interface are read from the driver's code
instead (§2.8–§2.14). The register-grade material
that *does* exist is the object-data interface the rendering pipeline consumes (§2.2), the on-chip pixel state
(§2.3), and the texture memory interface and its address organization (§2.4–§2.5), each documented by the
designers in patent text [8] [9] and, for the texture memory, independently confirmed on the shipped hardware
by pin-level measurement [15]. The control state the driver loads before a frame (§2.6) completes the set.

§2.2–§2.7 describe the design as its papers and patents present it; §2.8–§2.14 describe the interface the
shipped driver programs, which is what software actually sees. The two do not map one to one — the patent's
token stream is internal to the ASIC, while the driver writes packets, control programs and object records —
and the companion chip's own programming interface, if it has one beyond the three registers of §2.9, is
not in the evidence ([§6](#6-open-questions)).

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


### 2.8 PCI identity and configuration space

The host interface below is reconstructed from the card's own driver software: the PowerPC code of
"Apple QD3D HW Driver" (the native driver, `.GothamDriver`) and "Apple QD3D HW Plug-In", whose debug builds
keep their function names in the compilers' traceback tables [18]. Every value in §2.8–§2.14 is read from
that code (*observed in the driver*); where the code shows what the driver writes but not what the card does
with it, the text says *inferred*.

| Item | Value |
|---|---|
| Vendor / device ID | `0x106B` (Apple) / `0x0009`; the driver's `TheDriverDescription` matches the Name Registry device name `pci106b,9` |
| Class code | not checked by the driver |
| Revision ID | read per card; **2** grants 0x40000 texels of texture memory and 16-bpp destinations, any other value 0x10000 texels and 32 bpp only (§2.10) |
| BARs | exactly one 32-bit memory BAR: the driver insists that `assigned-addresses` holds one 20-byte entry with the memory-space bit set, and uses its `phys.lo` directly as the register base; the size field is never read |
| Expansion ROM | none required: the driver ships as an extension and matches by name |
| Command | the driver writes `0x16` — memory space, bus master, memory write and invalidate |
| `0x0C` | the driver writes `0x0000FF08` — latency timer 0xFF, cache line 8 words |
| `0x40` (vendor) | written 0, then `0xF0 \| (enabled − 1) << 2 \| index` — the card's position among up to four enabled cards (§3.8) |
| Interrupt | one line, INTA; the handler is installed through the Name Registry `driver-ist` property |

The meaning of the `0xF0` bits of register `0x40` is not established ([§6](#6-open-questions)).

### 2.9 Host-visible registers

The BAR exposes three 32-bit locations. Every access the driver makes is a 32-bit `lwz`/`stw` of a
big-endian word, followed by `eieio`; there are no byte-reversed accesses anywhere in the driver or the
plug-in [18].

| Offset | Name | Access | Function |
|---|---|---|---|
| `0x00` | Port | write | packet stream into the renderer's internal address space (§2.10) |
| `0x40` | DMA control | read/write | the pixel-output channel's control and status |
| `0x4C` | DMA command pointer | write | host-physical address of the pixel-output descriptor list (§2.14) |

The driver reads only `0x40`, and never touches `0x44`, `0x48` or `0x50`–`0x58`, the offsets where an Apple
DBDMA channel keeps its status, high pointer and select registers. Despite the DBDMA-like offsets, `0x40`
does not follow DBDMA semantics: it is read back directly, and writes carry no write-enable mask half.

DMA control bits:

| Bit | Mask | Meaning |
|---|---|---|
| 15 | `0x8000` | run (set in every start value) |
| 14 | `0x4000` | interrupt enable — must read 0 before a start, is ORed in to start a frame, and is expected set by the interrupt handler |
| 13 | `0x2000` | interrupt acknowledge: the handler reads the register and writes the value back with this bit set |
| 12 | `0x1000` | reset: the driver writes `0x1000`, waits 20 µs, writes 0, waits 10 µs |
| 11, 10 | `0x0C00` | 16-bpp output, set together when the destination has two bytes per pixel |
| 9–0 | — | never written |

The reset is performed before every frame. It does not clear the object cache: the driver re-initialises the
cache only when its mode changes, so the cache contents survive the per-frame reset (*inferred*).

### 2.10 The packet port and the internal address space

Words written to the port are packets whose first word carries an opcode in bits 31–29; the remaining bits
are a **byte address** in the renderer's internal space [18]:

| Opcode | Header | Following words | Meaning |
|---|---|---|---|
| 1 | `0x2000_0000 \| address` | one data word (none for the strobe registers `0x28`/`0x2C`) | write one word |
| 2 | `0x4000_0000 \| address` | `count − 1`, then `count` data words | block write to consecutive words |
| 3 | `0x6…` | — | a renderable object; appears only in control programs (§2.13) |

The same encoding is what the renderer parses out of a control program (§2.12), so the port is a host-side
entry into one command decoder (*inferred*).

| Internal address | Contents |
|---|---|
| `0x000000`–`0x0000FF` | renderer registers (§2.11) |
| `0x400000`–`0x41FFFF` | object cache, 128 KB |
| `0x800000`– | texture memory, addressed in 4-byte units: one 32-bit `0xAARRGGBB` port word per texel |

At open, after a timeout, and when the cache mode changes, the driver resets the card, zeroes
`0x400000`–`0x401FFF` with four 512-word opcode-1 sequences, and builds a free list of **1706 nodes of
72 bytes** across the rest of the cache (`0x2000`, `0x2048`, … `0x1FF88`, the first word of each the address
of the next, the last 0), then writes register `0x34 = 0x2000`. In "cache overflow" mode it writes
`0x34 = 0` instead and builds no list. 1706 is also the object count the plug-in budgets per frame (§3.2).

Texture upload is opcode-2 block writes of one row each: an `n × n` level is sent row by row **bottom row
first**, to `levelBase + slot · n² · 4 + row · n · 4`, and the data is broadcast to every enabled card. The
driver reports the texture capacity as 0x40000 texels when the lowest revision among the enabled cards is 2,
else 0x10000; 0x40000 texels × 16 bits is the 512 KB store, so the card packs each 32-bit port word to a
16-bit texel (*inferred*).

### 2.11 Renderer registers

Written through opcode-1 packets, by the driver directly or by the plug-in through the driver; none is ever
read back [18].

| Address | Name | Value |
|---|---|---|
| `0x00` | Mode | `(index << 8) \| flags`; index 0–3 in bits 9–8. Flags: `0x0008` destination is 16 bpp; `0x0400` the destination's base address has bit 1 set (a halfword-aligned 16-bpp destination); `0x0800` perspective depth — the record depth fields hold 1/w and texture coordinates u/w, v/w; `0x1000` always set |
| `0x04`–`0x07` | Y | control programs only: `(Ynext << 16) \| Ycur` for the card whose index equals the address's low two bits (§2.12) |
| `0x08` | Background | ARGB8888, packed from the RAVE background colour each frame |
| `0x0C` | Texture configuration | `(n256 << 8) \| ((page of the first 128-map − n256) & 0xFF)`, 0 when no maps are loaded; with the plug-in's allocation the low byte equals `16 − n128 − n256` |
| `0x20` | Go | host-physical address of the frame's control program; the last write of a frame start |
| `0x24` | CSG equation | the 32-entry CSG truth table (§3.5), unchanged from the RAVE state: bit *s* is the result for inside-state *s*, in which operand *n* is bit *n* — QuickDraw 3D's own constants, A ∩ B = `0x88888888` and A − B = `0x22222222`, fix the order [20] |
| `0x28` | Scanline start | strobe, no data |
| `0x2C` | Scanline end | strobe, no data |
| `0x30` | Width − 1 | output pixels per scanline, minus one |
| `0x34` | Free-list head | `0x2000`, or 0 in cache-overflow mode |
| `0x48`–`0x68` | Level base 0–8 | internal address of mip level *L* (256² down to 1²) of all resident maps |
| `0x80`–`0xBC` | Slot class 0–15 | per slot: 3 for a 256² map (nine levels), 2 for a 128² map (eight levels) |

Texture memory is laid out **by mip level, not by map**: level 0 holds the 256² maps' top levels, and each
lower level array holds one page per resident map, 256² maps first. With `n256` and `n128` maps resident:

```
base[0] = 0x800000
base[1] = base[0] + n256 · 256² · 4
base[L+1] = base[L] + s² · (n256 + n128) · 4      for s = 128 >> (L−1), L = 1..7
```

A 256² map occupies pages 0, 1, … upward and its slot equals its page; 128² maps occupy pages 15, 14, …
downward, their slot is `page − (register 0x0C & 0xFF)`, and a 128² map's own level 0 is global level 1. A
texel of level *L* of the map in slot *k* is at `base[L'] + (k · s² + row · s + column) · 4` with row 0 the
image's bottom row (§2.10) and `L'` the global level.

### 2.12 The control program: control blocks and the Y registers

A frame is one singly linked list in host memory that interleaves **control blocks** and **object records**
(§2.13); the card fetches it by bus mastering from the address written to register `0x20` [18]. Every record
starts with `N` link words, `N` being the number of enabled cards; card *k* follows the word at offset
`4k`, and the plug-in writes the same address into all `N`. A link of 0 ends the list (*inferred*). The
record's first word after the links tells the two kinds apart: an object header has opcode 3 in bits 31–29;
a control block does not.

A control block is seven command words after the links: opcode-1 register writes (with their data word,
except for the strobes), and zero words in unused slots. The plug-in builds one skeleton program per draw
context, for `S = height + N` scanlines:

| Block | Contents |
|---|---|
| initial | `Y = (Y(k) << 16) \| 0xFFFF` for each card *k* (next = its first scanline, current = none), and `0x30 = width − 1` |
| A, per scanline group *i* | `Y` for each card, then **scanline start** |
| B, per scanline group *i* | five zero words, **scanline end**, one zero word |

Y values are 13.3 fixed point at the pixel centre, `(line << 3) | 4`, with `0xFFFF` for "none". For card
*k* of *N* in group *i*, `current = N·i + k` and `next = N·(i+1) + k`, each replaced by `0xFFFF` when not
below `S`. The program therefore runs one scanline past the bottom of the drawable for each card, with
`next = 0xFFFF`. Objects are linked in after block B of the group before the one in which they first cover a
pixel centre, so the card meets every object just before the scanline where it becomes active; within one
group they appear in reverse submission order (the plug-in pushes them at the head). With one card the
skeleton for 640×480 holds 963 control blocks.

### 2.13 Object records

Every object record is `N` link words, a header word and a payload, all big-endian [18]. The header:

| Bits | Mask | Meaning |
|---|---|---|
| 31–29 | `0xE000_0000` | 3: renderable object |
| 27 | `0x0800_0000` | set on every object the plug-in emits; meaning unknown |
| 26 | `0x0400_0000` | back-facing (the RAVE triangle flag) |
| 25 | `0x0200_0000` | textured triangle |
| 24 | `0x0100_0000` | CSG operand; the operand number 0–4 is in bits 2–0 |
| 23 | `0x0080_0000` | bitmap tile |
| 21 | `0x0020_0000` | opaque: all three vertex alphas are 255 and the object is not a CSG operand |
| 7–4 | `0x0000_00F0` | texture page (textured triangles) |

Payload offsets below count from the header word.

**Gouraud triangle** (bits 25 and 23 clear; 52 payload bytes after the header used):

| Offset | Format | Content |
|---|---|---|
| `+4`, `+8`, `+12` | `x << 16 \| y`, 13.3 unsigned each | vertices 0, 1, 2: `floor(coordinate · 8)` |
| `+16`, `+20`, `+24` | IEEE single | depth of each vertex: 1/w with mode flag `0x0800`, else z |
| `+28`, `+32`, `+36` | A, R, G, B bytes | vertex colours, `floor(c · 255.9)` |

**Textured triangle** (bit 25):

| Offset | Format | Content |
|---|---|---|
| `+4`–`+24` | as above | vertices and depths |
| `+28 + 4i` | byte, nibble pair, 16 bits | vertex *i*: alpha `floor(a · 255.9)`; `(v_low4 << 4) \| u_low4`; the top 16 bits of an IEEE single holding the vertex's mip-level footprint (0 when the map is not mipmapped) |
| `+40 + 4i` | 16 bits, 16 bits | the top 16 bits of the IEEE singles **v** and **u**; with the nibbles above, 20-bit floats (sign, 8-bit exponent, 11-bit mantissa) |
| `+52 + 4i` | bytes 2 and 3 | diffuse intensity `kd = floor((kd_r + kd_g + kd_b) · 42.65)`, a 1.7 fixed-point value (128 ≈ 1.0), and specular `ks = floor((ks_r + ks_g + ks_b) · 85.3)`, 0.8 fixed point |

Vertex RGB is not sent for textured triangles: shading is one grey diffuse and one grey specular intensity
per vertex. u and v are in normalised map units; with mode flag `0x0800` they are u/w and v/w and the card
divides per pixel, otherwise the plug-in has divided them. The footprint is
`max(|∂(u/w)/∂x · q − ∂q/∂x · u/w|, …)` over x, y and both coordinates — the texel-space derivative scaled
by q², which the card must divide by q² per pixel to pick a mip level (*inferred*). In the affine case one
value, `max(|∂u/∂x|, |∂u/∂y|, |∂v/∂x|, |∂v/∂y|)`, is repeated in all three vertices.

**Bitmap tile** (bit 23): one 32 × 12-pixel, one-bit tile of a RAVE bitmap.

| Offset | Format | Content |
|---|---|---|
| `+4` | IEEE single | depth |
| `+8` | `x << 16 \| y`, 13.3 | tile left and top, whole pixels |
| `+12`, `+16` | ARGB8888 | the drawing colour, twice |
| `+20 + 4r` | 32 bits | row *r* (0–11), bit 31 the leftmost pixel |

The plug-in moves a partly off-screen tile fully on-screen and zeroes the rows that fall outside, so the card
always receives whole tiles inside the drawable. Points arrive as one small Gouraud triangle and lines as two
(a one-pixel-wide quad).

Coverage follows the plug-in's bucketing: a triangle covers the scanlines whose centres lie in
`(yMin, yMax]` in 13.3 units — `first = (yMin + 4) >> 3`, `last = (yMax − 4) >> 3` — and covers none when
`last < first`. The horizontal sampling rule is not visible in the software ([§6](#6-open-questions)).

### 2.14 Pixel-output descriptors

The pixel-output channel consumes a list of 16-byte **big-endian** descriptors from the address in DMA
command pointer (`0x4C`) [18]:

| Word | Bits | Content |
|---|---|---|
| 0 | 31–28 | 0 |
| 0 | 27–24 | command: 0 write, 1 skip, 8 stop |
| 0 | 23–16 | 0 |
| 0 | 15–0 | byte count |
| 1 | — | host-physical destination address (write only; set but ignored on skip) |
| 2, 3 | — | 0 |

The descriptors carry no source address: the source is the renderer's pixel stream, scanline after scanline,
each `width` pixels wide (register `0x30`) in the destination format — `xRGB` 32-bit words, or 16-bit
pixels when DMA control bits `0x0C00` and mode flag `0x0008` are set. A write descriptor copies `count`
bytes of the stream to its address, a skip discards `count` bytes, stop ends the list. The plug-in's own
CPU model of the channel, which it uses to copy a back buffer to the screen, advances the source by `count`
on both write and skip and wraps to the next source row after `width · bytes per pixel` bytes.

The plug-in builds one list per card, covering scanlines `k, k + N, …`. Within each scanline it emits one
write per run of pixels inside the window's visible region and one skip per run outside it, or per run past
the destination's width; a card with one scanline fewer than the others gets one padding skip line; every
list ends with a stop. Destinations are either the screen's framebuffer (single-buffered contexts, clipped
by the visible region) or a physically contiguous back buffer in host memory (double-buffered contexts,
unclipped), which the plug-in then copies to the screen with its CPU model of the same list.

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
on the way out. The shipped driver also programs 16-bit output for thousands-of-colours destinations (DMA
control `0x0C00`, mode flag `0x0008`, §2.9, §2.11), on revision-2 cards only [18]; the pixel-output
descriptor list that steers the stream is §2.14.

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

The driver settles how two *cards* divide a frame: by **interleaved scanlines** [18]. Every card receives
the same control program; each is told its position through configuration register `0x40` and the index
in mode register bits 9–8, latches only its own Y register writes (§2.12), and owns its own descriptor list
covering scanlines `k, k + N, …` (§2.14). Up to four cards are enabled. The ~80% scaling observed with two
cards [15] is therefore not a partition imbalance; the per-object setup every card repeats and the shared
PCI bus are the candidates.

### 3.9 Interrupts

The design intent on record is minimal: the rendering pipeline is to "interrupt the processor only when it
has finished processing a frame", leaving the CPU free to work on the next frame's geometry [7]. The
shipped card does exactly that: one interrupt per card per frame, at the end of its pixel-output list,
gated by DMA control bit 14 and acknowledged by writing bit 13 (§2.9, §3.11) [18]. There is no other
interrupt source and no status register beyond DMA control.

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


### 3.11 The frame protocol

From the plug-in's frame end to the interrupt, the driver drives every card the same way [18]:

1. **Lock**: DMA control `0x1000`, 20 µs, `0`, 10 µs — before every frame.
2. If the context or cache mode changed: re-initialise the object cache (§2.10).
3. If the resident texture set changed: upload every map, then write the level bases (`0x48`–`0x68`) and
   slot classes (`0x80`–`0xBC`).
4. Background (`0x08`), CSG equation (`0x24`), texture configuration (`0x0C`).
5. Per card: mode (`0x00`) with the card's index, DMA command pointer, a read of DMA control checking that
   bit 14 is clear, then DMA control `= 0x8000 [| 0x0C00] | 0x4000`.
6. 100 µs, then the driver marks every card as running and writes the control program's physical address
   to register `0x20` of each card.

The card then walks the program, renders each of its scanlines and streams them through the descriptor list;
at the list's stop it interrupts. The handler reads DMA control, writes it back with `0x2000`, warns if bit
14 is clear, and clears the card's running flag; when no enabled card is still running it reports the frame
done to the plug-in with the measured render time. A completion that arrived before step 6 had marked the
card running would be lost, so the interrupt must not be raised synchronously with the register `0x20`
write (*inferred*).

A watchdog started with each frame fires after `max(3 s, 10 × the longest render so far)`; it resets the
cards, re-initialises the object cache and reports a timeout, and the plug-in then drops the frame.

The plug-in keeps two control programs and two object heaps and alternates between them, so it builds frame
*n* + 1 while the card renders frame *n*; it waits for frame *n* to finish before starting *n* + 1.

## 4. Programming model

### 4.1 QuickDraw 3D and where the card hooks in

QuickDraw 3D is a high-level, cross-platform 3D API with a scene graph and retained objects, a common file
format (3DMF) for exchanging models, and "an extensible, plug-in rendering architecture" with "transparent
access to graphics accelerators — no changes to your code are needed to take advantage of
[QuickDraw 3D]-compliant boards" [17]. Applications normally render through the **interactive renderer**
interface, which selects between the software renderer and hardware acceleration without application
involvement [15] [17].

The card integrates at that level — inside the QuickDraw 3D renderer stack. With QuickDraw 3D 1.0.3 the
plug-in sat behind a separate "QuickDraw 3D Accelerator" library; from 1.5 the plug-in is formally a RAVE
engine, but one that disables itself as soon as it has registered, so only QuickDraw 3D's interactive
renderer, which enables it explicitly, ever draws through it (§4.7) [18]. RAVE is the interface of every
later Mac 3D accelerator ([rage-128.md](rage-128.md) §1, [voodoo1.md](voodoo1.md) §1); RAVE titles enumerate
engines the ordinary way and never find this one. The consequences observed across the card's whole
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
FastFeatures Line/Gouraud/Texture/TextureHQ) [15]. The descriptor is the plug-in's RAVE gestalt (§4.7): the
plug-in registers "Apple PCI Hardware" with the RAVE manager and answers vendor 0 and engine ID −1, the
`kQAEngine_AppleHW` value [18]; the probe also omits CSG, which the plug-in does report.

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

### 4.7 The RAVE engine in QuickDraw 3D 1.5 and later

From QuickDraw 3D 1.5 the plug-in is a **RAVE engine**: it registers itself with the RAVE manager as
"Apple PCI Hardware" (vendor 0, engine ID −1, revision 1), and RAVE's own engine-information sample lists
it ahead of the software engine [18] [19]. The plug-in then **disables the engine it has just registered**,
so the engine is invisible to a RAVE application that enumerates engines in the ordinary way; QuickDraw 3D's
interactive renderer enables it explicitly with `QAEngineEnable`. That, not a missing interface, is why the
RAVE titles of §4.5 never reach the card. The 1.0.3 release used a different arrangement: a separate
"QuickDraw 3D Accelerator" shared library between QuickDraw 3D and the plug-in [18].

The engine's gestalt answers: optional features `0xCB7` (deep Z, texture, high-quality texture, blend,
alpha blend, Z-sorted, no-clear, CSG — not texture colour, not antialiasing), fast features `0x5E`
(Gouraud, texture, high-quality texture, blend, Z-sorted), and texture memory as **free texels**, not bytes:
`capacity − n128 · 0x5555 − n256 · 0x15555`. Of the RAVE state tags the plug-in honours only the background
colour, perspective Z, the texture, the CSG tag and the CSG equation; Z function, width, antialiasing, blend
mode, texture filter and texture operation are stored and ignored, and tags above 15 are not supported.
Bitmaps are converted to one bit per pixel with QuickDraw's dithering copy and drawn as tiles (§2.13) in the
single vertex colour.

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
9. **RAVE blindness.** The plug-in registers its RAVE engine and immediately disables it, so RAVE-only
   applications never select the card; only QuickDraw 3D enables it (§4.1, §4.7) [15] [18].
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

1. **PCI identity details.** The class code and the BAR size are invisible to the driver (§2.8); a
   configuration-space dump of a real card would settle both, and whether any expansion ROM is fitted.
2. **What the driver does not exercise.** The read-back bits of DMA control while a frame runs; whether the
   channel writes status back into descriptor words 2–3; the descriptor bits the plug-in always clears;
   the `0xF0` bits of configuration register `0x40`; mode flags `0x1000` and `0x0400` beyond their
   conditions (§2.9–§2.11, §2.14).
3. **The companion chip.** The second, TI-manufactured ASIC's identity and division of labor — PCI bridge,
   command processor, or both — is not established; the architecture papers name the *functions* (bus
   attachment, object fetch) but no document maps them to the physical part.
4. **Token-format fidelity.** Whether the shipped card consumes the §2.2 token stream exactly as the patent's
   preferred embodiment defines it (field widths, TokenID assignments, control OpCodes) is unverified — no
   logic-analyzer capture of the object-data interface exists.
5. **Depth format.** 24-bit stored Z (1994 design) versus 32-bit Z endpoints (patent tokens) versus 25-bit
   floating point (1997 successor): the shipped card's working depth format and its tie behaviour are
   unconfirmed (§2.3, quirk 1).
6. **The multi-card bottleneck.** The partition is scanline interleave (§3.8); what limits two cards to
   ~80% of a doubling — repeated per-object setup or bus contention — is not measured.
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

11. **Pixel-exact rules.** The horizontal edge rule (the vertical one is fixed by the plug-in's bucketing,
    §2.13), colour interpolation and rounding, the texture modulation formula behind the 1.7/0.8 kd/ks
    encodings, the 16-bit texel packing, how the card weighs a non-opaque layer's colour (as RAVE's default
    premultiplied blend, or by its alpha), and which equal-depth layer wins — none is visible in the driver
    (§2.13, quirk 11).
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
16. **Object-record details.** Header bit 27, always set; how the card determines a record's length (most
    likely from bits 25 and 23); whether it reads the unwritten tail of a Gouraud record; whether it
    rewrites link words while rendering (the plug-in resets every object's links after a partial flush,
    which would only matter if it did); and what the card does in cache-overflow mode (§2.10, §2.13).

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
18. Apple Computer, Inc., *Apple QD3D HW Driver* (native driver `.GothamDriver`) and *Apple QD3D HW
    Plug-In* — the 1.5.4 debug builds in the QuickDraw 3D 1.5.4 SDK, the 1.6 debug builds in the
    QuickDraw 3D RAVE 1.6 DDK, and the 1.0.3 retail builds on the card's CD; PowerPC disassembly, with
    function names from the builds' traceback tables (`DriverInitializeCmd`, `GrPCIDoIt_StartRender`,
    `GmInterruptServiceRoutine`, `ZGrControlProgram_Build`, `ZGrDrawTriGouraud`, `ZGrDrawTriTexture`,
    `ZGrDrawBitmap`, `ZgrDBDMAProgram_BuildScript`, `ZGrDBDMA_Engine`, `ZGrTextureLoad`,
    `ZGiTexture_InitMapPageBase`, `ZGrEngineGestalt` and others).
19. Apple Computer, Inc., *RaveEngineInfo* and *Sneak3View* — sample programs of the QuickDraw 3D 1.6 SDK
    and the QuickDraw 3D RAVE 1.6 DDK: an engine lister that prints every registered RAVE engine's gestalt
    answers, and a test harness that draws the same scenes through two engines side by side.
20. Apple Computer, Inc., *QuickDraw 3D* SDK interfaces, `QD3DRenderer.h` (QuickDraw 3D 1.1 and later) — the
    `TQ3CSGEquation` constants (`kQ3CSGEquationAandB = 0x88888888`, `kQ3CSGEquationAandnotB = 0x22222222`,
    `kQ3CSGEquationnotAandB = 0x44444444`) and `Q3InteractiveRenderer_SetCSGEquation`, whose value the
    plug-in hands to register `0x24` unchanged.
