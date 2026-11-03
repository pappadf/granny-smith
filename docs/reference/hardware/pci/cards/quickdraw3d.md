# The Apple QuickDraw 3D Accelerator Card ("Gotham")

**Contents:**

1. [Overview](#1-overview) — what the card is, the board and its silicon, the system at a glance, the White
   Magic lineage, host machines, software versions, the evidence base
2. [Register file](#2-register-file) — the host interface in summary, PCI configuration space, the BAR
   registers, the packet port, the renderer registers, the object cache, texture memory, the control
   program, object records, pixel-output descriptors, and the internal formats of the published design
3. [Behaviour](#3-behaviour) — the frame, the program walk, rasterization, visibility, compositing, CSG,
   texturing, bitmap tiles, pixel output, completion and the watchdog, reset, multiple cards, timing
4. [Programming model](#4-programming-model) — the software stack, installation and discovery, driver
   initialization, the driver/plug-in protocol, the frame sequence, the RAVE engine, building a frame,
   textures, bitmaps, observed behaviour and performance
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

[Appendix A. Granny Smith implementation notes](#appendix-a-granny-smith-implementation-notes)

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

The card's place in the host software stack is the most consequential fact about it. Its driver and plug-in
present it to QuickDraw 3D as a RAVE drawing engine, "Apple PCI Hardware", but the plug-in **disables the
engine as soon as it has registered it**; QuickDraw 3D's own renderers enable it explicitly, while a RAVE
application enumerating engines the ordinary way never finds it (§4.1, §4.6) [18] [22]. In practice only
applications that render through QuickDraw 3D's high-level interface use the card, and every RAVE title of the
later Mac game catalog runs without it (§4.11) [15].

### 1.2 The board and its silicon

The card is built around **two custom Apple ASICs** — a rendering engine "based on state-of-the-art O.5µ
technology" (0.5 µm) plus a second custom part — with 512 KB of high-speed SRAM texture memory and a 128 KB SRAM
cache, no framebuffer, and no video connectors [1] [3]. Contemporary press reporting from its launch window
describes the same population: "a rendering engine and two special ASICs as well as 128 KB static RAM cache
and 512 KB static main memory" [4]. The physical record of the boards (front/rear photographs, part and
assembly numbers on the silkscreen) identifies the card as Apple part **M4142**, assembly **820-0739** and a
-**A** revision, with earliest production-week markings around week 35 of 1995 (*observed* on the board
photographs of the research record [15]; the part numbers are not corroborated by an Apple document).

| Block | What it is |
|---|---|
| Rasterizer ASIC | the modified-scanline rendering engine of §3, clocked at 40 MHz [5]; on final production silicon it reportedly carries the marking **343S1167** (*community-reported* from a teardown [15]) |
| Companion ASIC | one TI-manufactured part next to the rasterizer; by elimination the PCI interface — the BAR, the packet port, the bus-master fetch of the control program and the pixel-output DMA channel of §2 are all on the host side of the renderer — but no document names it (*inferred*; [§6](#6-open-questions)) |
| Texture SRAM | 512 KB as two banks of 131 072 × 16-bit words holding even and odd texels (§2.7) [9] [15] |
| Object cache SRAM | 128 KB holding the renderer's active-object list (§2.6) [5] [15] |

The logic is a 3.3 V design, clocked from a 40 MHz oscillator; community overclocking ran it reliably at
44 MHz and, with a faster oscillator and 12 ns cache SRAM, at up to 54 MHz (§3.13) (*observed*) [15]. The card
dissipates enough heat to matter: in a packed Power Macintosh 7500 both observed cards ran hot enough to need
added coolers, and in a 9500 one of two installed cards ran hot while the other stayed merely warm
(*observed*) [15].

### 1.3 The system at a glance

The card is a **deferred, whole-frame scanline renderer driven entirely from host memory** [5] [18]:

1. The host does all geometry: QuickDraw 3D's renderer transforms, lights and clips, and the card's plug-in
   turns each screen-space triangle into an **object record** — 13.3 fixed-point vertices, float depth, ARGB
   colour or texture coordinates — computing no slopes or gradients (§2.9, §4.7).
2. The plug-in links the records into a **control program** in physically contiguous host memory, placing each
   object just before the scanline where it first becomes active, among **control blocks** that sequence the
   scanlines (§2.8).
3. The driver resets the card, uploads textures through the **packet port** into the card's texture SRAM,
   writes a handful of renderer registers, points the **pixel-output DMA channel** at a descriptor list, and
   writes the control program's physical address to the renderer's **go register** (§2.3–§2.5, §4.5).
4. The card fetches the program by bus mastering, keeps the active objects in its object cache, renders each
   scanline in 16-pixel segments with per-pixel depth sorting into four layers, CSG and front-to-back
   transparency, textures only the visible fragments, and **writes the finished pixels over PCI** into the
   screen's VRAM or a host back buffer, as the descriptor list directs (§3).
5. At the end of the list the card interrupts; the driver tells the plug-in the frame is done (§3.10).

Up to four cards cooperate on one frame, each rendering every *N*-th scanline from the same program (§3.12).

### 1.4 The White Magic lineage

The card is the shipped product of an Apple research line that ran from the late 1980s inside Apple's
advanced research group, through three documented hardware generations. The published papers and the patent
family describe each generation; the naming is *White Magic* for the project, *Gotham* for the card
(*community-reported, consistent across the research record* [15]; the driver's own messages also call the
card family "White Magic" [18]):

| Generation | Date | Interface | Architecture | Performance | Textures |
|---|---|---|---|---|---|
| White Magic prototype | 1992 | 68040 Processor Direct card in a Quadra [6] | two 0.8 µm chips at 40 MHz; scanline Z-buffer; 80 bits/pixel on-chip (32 Z + 8 shadow + 10 bits each aRGB); 640-pixel scanline [6] | 40M alpha-blended Z-buffered pixels/s; 880K triangles/s in a 4-wide parallel configuration [6] | none [6] [15] |
| Gotham | 1994 (prototype), late 1995 (product) | PCI card for the Power Macintosh [1] [5] | single rendering ASIC at 40 MHz; 16-pixel scanline segments; four-layer Z-ARGB state on-chip at 480 bits/pixel; CSG evaluator; deferred trilinear texturing [5] | 20M pixels/s untextured, 10M pixels/s trilinearly textured, 120K triangles/s [1] [5] | trilinear mipmapped, 256×256 max [1] [9] [15] |
| Unshipped successor | 1996/97 designs, never productized | PCI or AGP; rasterizer + I/O-interface ASIC pair | 16×32-pixel tiles; modified A-buffer with an 8-bit staggered subpixel mask; 25-bit floating-point depth; on-chip texel caches; up to 4 rasterizers per I/O ASIC [7] [14] | 100M pixels/s at 100 MHz; 2M texture-mapped triangles/s; hardware antialiasing at a 30–40% penalty [7] | up to 2048 texels/side, 16 or 32 bits/texel [7] |

The three generations share one architectural idea: keep the per-pixel rendering state small enough to live
on-chip — a scanline in 1992, a 16-pixel segment in 1994, a 16×32 tile in 1997 — so that the depth buffer and
compositing state never need off-chip bandwidth, and render front-to-back so that transparency and CSG fall
out of the same per-pixel Z machinery [5] [6] [7]. The 1997 design study states the continuity explicitly: it
"describes a low cost hardware accelerator for rendering 3D graphics with antialiasing... based on a previous
architecture" — the 1994 system — and the two papers share authors [5] [7].

### 1.5 Host machines

The card works in "any Power Macintosh or Macintosh Performa computer with a PCI slot", with a PowerPC
processor, 16 MB of RAM, and System 7.5.2 with System Enabler 1.1 or later; Apple's examples are the Power
Macintosh 7200, 7500, 8500 and 9500 [1]. The display must be set to **thousands or millions of colours** —
"QuickDraw 3D will run if you choose another display format, but rendering will not be accelerated" — and
Apple recommends turning Virtual Memory off on 16 MB systems [1]; thousands of colours needs a revision-2
card (§4.6). The PCI environment itself — configuration cycles, memory and I/O windows, the big-endian host to
little-endian bus seam, the Open Firmware startup contract — is [pci.md](../pci.md)'s (§2, §4). The plug-in
needs `DriverServicesLib`, so the card is a PCI Power Macintosh device only [22].

Observation places the card in two of the TNT-family machines ([tnt.md](../../../machines/tnt/tnt.md) §1.2):
the **Power Macintosh 7500** ([pm7500.md](../../../machines/tnt/pm7500.md) §1.1), where two cards were run
together, and the **Power Macintosh 9500** ([pm9500.md](../../../machines/tnt/pm9500.md) §1.1) with its six
PCI slots — three on the processor bus and three behind the PCI-PCI bridge
([bandit.md](../../../machines/tnt/bandit.md) §1.4) [15]. The framebuffers it accelerates in those machines
are ordinary display hardware: the TNT platform's built-in video ([tnt.md](../../../machines/tnt/tnt.md)
§2.4), the 9500's bundled Apple mach64 GX display card ([mach64.md](mach64.md) §1.2), or a third-party PCI
board such as the TwinTurbo-based 9600 Graphics Accelerator ([twinturbo.md](twinturbo.md) §1.3) — the card
writes to all of them alike, which is its point [1]. Two behavioural facts of the platform pair matter:

- The card works in any of the six slots, but renders about **10–15% faster in the bus segment where the
  framebuffer lives** (*observed* on a 9500) [15] — the pixel output (§3.9) crosses PCI to the host
  framebuffer, so a bridge hop on either end costs bandwidth ([pci.md](../pci.md) §3.3).
- A single card reached 40–50 fps and a two-card setup 70–85 fps in the Gerbils! test application on a 9500
  upgraded with a 375 MHz PowerPC G3 (*observed*) [15].

### 1.6 Software versions

The card's software shipped in four generations, all driving the same hardware protocol: every hardware
constant — configuration values, packet tags, the reset pulse, the object-cache layout, the frame start
order, the interrupt acknowledge — is identical from the 1995 retail driver to the 1999 one [18].

| Release | Date | Driver | Plug-in | Integration |
|---|---|---|---|---|
| QuickDraw 3D 1.0.3 (the card's CD) | September 1995 | optimized; traceback names | engine-method tags 0–9, gestalt revision 0, draw methods stored straight into the draw context | behind the "QuickDraw™ 3D Accelerator" 1.0.3 library, the RAVE manager before the name |
| RAVE 1.5 SDK (1.5d22) | November 1996 | debug build with assertion messages | tags 0–13 (colour tables), revision 1, `QARegisterDrawMethod`, display-mode notices | RAVE 1.5 |
| QuickDraw 3D 1.5.4 SDK | February 1998 | as 1.5d22 (one changed error code) | adds the QuickDraw draw-notification guard (§4.6) | RAVE 1.5.4 |
| QuickDraw 3D 1.6 / RAVE 1.6 DDK (1.6e1) | April 1999 | rebuilt with another compiler; no debug strings | behaviour as 1.5.4; dead code stripped | RAVE 1.6 |

The driver's own version stayed "1.0a6" throughout; only its container version changed [18]. The debug builds
(1.5d22 and 1.5.4) keep their function names in traceback tables and print assertion messages through
`SysDebugStr`, which makes them the reference for this page.

### 1.7 Evidence base and its limits

The page rests on three kinds of evidence:

- **The shipped software.** The card's host-visible interface — PCI identity, the BAR, the packet port, the
  renderer registers, the control-program, object-record and descriptor formats, the frame protocol — is read
  from the PowerPC code of Apple's "Apple QD3D HW Driver" and "Apple QD3D HW Plug-In" in all four software
  generations, together with the QuickDraw 3D and RAVE system libraries that load them [18] [23], read against
  Apple's driver and RAVE documentation [21] [24] and Apple's developer Q&As [22]. This establishes exactly
  what the software writes and expects; what the card does with a value the software never checks is marked
  *inferred*.
- **The designers' publications.** Three SIGGRAPH papers by the chip's designers describe the 1992 prototype,
  the 1994 Gotham architecture and the 1997 successor [5] [6] [7], and the Apple patent family covers the
  scanline pipeline and its token interface, the texture memory organization, the object cache and the
  multi-layer Z-buffer [8]–[14]. They describe the *design*; where a claim concerns the shipped card rather
  than the published design it is marked *inferred - unverified* unless other evidence confirms it.
- **The hardware investigation.** A sustained public investigation of the physical card [15] — board
  photographs, oscilloscope tracing of the rasterizer's SRAM interface, overclocking, dual-card experiments,
  application compatibility testing and benchmarks on Power Macintosh 7500 and 9500 hardware — is the
  evidence for the physical board, the texture memory's pinout, the 16-bit texel format and the card's
  observed behaviour. Apple's own product documentation [1] [2] [3] and contemporary press [4] [16] [17]
  supply the product facts.

No register documentation for the card was ever published, there is no expansion ROM, and no bus trace of a
real card exists in the public record. The consequence is stated honestly: §2–§4 are complete to the grade of
what the shipped software does — enough to run Apple's unmodified driver and plug-in against an emulated card
— while the **pixel-exact rules** the software cannot observe (edge rules, interpolation and rounding, the
texture modulation formula) are inferences listed in [§6](#6-open-questions).

## 2. Register file

### 2.1 The host interface in summary

Software reaches the card through four layers [18]:

| Layer | Where | Contents | Section |
|---|---|---|---|
| PCI configuration space | config cycles | the standard header plus one vendor register, `0x40` (card position) | §2.2 |
| The BAR | one 32-bit memory BAR | three 32-bit registers: the packet port, DMA control, DMA command pointer | §2.3 |
| The renderer's internal space | written through the packet port | renderer registers, the 128 KB object cache, the texture SRAM | §2.4–§2.7 |
| Host memory | read and written by the card as a bus master | the control program and object records (read), the pixel-output descriptor list (read), the destination pixels (written) | §2.8–§2.10 |

Nothing on the card is ever read by the software except DMA control. The card's internal formats as the
design papers and patents describe them — the pipeline's tokens, the per-pixel layer store, the global mode
— are documented separately in §2.11; they are what the rasterizer computes with, not an interface the host
programs.

### 2.2 PCI configuration space

| Item | Value |
|---|---|
| Vendor / device ID | `0x106B` (Apple) / `0x0009`; dword 0 reads `0x0009106B`, which the driver checks at initialization, failing with `controlErr` on a mismatch |
| Device name | `pci106b,9` — the Name Registry node name Open Firmware generates, and the name in the driver's `TheDriverDescription` |
| Class code | not checked by the software |
| Revision ID | read per card; **2** on the shipped revision. A minimum revision of 2 across the enabled cards gives 0x40000 texels of texture memory and 16-bpp destinations; any lower revision 0x10000 texels and 32 bpp only |
| Subsystem IDs | never read |
| BARs | exactly **one 32-bit memory BAR**: the driver insists that `assigned-addresses` holds one 20-byte entry whose space code is memory, and uses its `phys.lo` as the register base. Its size is never read; the software touches offsets `0x00`–`0x4F` |
| Expansion ROM | none: the driver ships as an extension and matches by name, and an assigned ROM range would lengthen `assigned-addresses` and make the driver refuse the card |
| Interrupt | one line, INTA, installed through the Name Registry `driver-ist` property |

The driver makes exactly these configuration accesses, per card, at initialization [18]:

| Order | Access | Offset | Value | Meaning |
|---|---|---|---|---|
| 1 | write | `0x40` | `0x00000000` | clear the position register |
| 2 | read | `0x00` | expect `0x0009106B` | identity |
| 3 | read | `0x08` | keep the low byte | Revision ID |
| 4 | write | `0x0C` | `0x0000FF08` | cache line size 8 words (32 bytes), latency timer `0xFF` |
| 5 | write | `0x04` | `0x00000016` | command: memory space, bus master, memory write and invalidate; status written as 0 |
| 6 | write | `0x40` | `0`, then `0xF0 \| (enabled − 1) << 2 \| index` | the position register, for every enabled card |

**Register `0x40` — position.** Bits 1–0 are the card's index among the enabled cards, bits 3–2 the number of
enabled cards minus one, bits 7–4 are always `0xF` when the card is enabled, bits 31–8 are 0. The driver
rewrites it on every enabled card whenever the set of enabled cards changes; a disabled card keeps the 0 from
step 1. It is the only vendor register the software touches; the meaning of the `0xF` nibble is open
([§6](#6-open-questions)). The BARs, the ROM BAR and the interrupt line are left to Open Firmware.

### 2.3 The BAR registers

The BAR exposes three 32-bit locations. Every access the software makes is a 32-bit `lwz`/`stw` of a
big-endian word followed by `eieio`; no build of the driver or plug-in contains a byte-reversed access [18].
The card therefore presents its registers big-endian to the processor (or the bridge swaps lanes so that it
appears to).

| Offset | Name | Access | Function |
|---|---|---|---|
| `0x00` | Port | write | the packet stream into the renderer's internal space (§2.4); never read |
| `0x40` | DMA control | read/write | the pixel-output channel's control and status; the only register the software reads |
| `0x4C` | DMA command pointer | write | host-physical address of the pixel-output descriptor list (§2.10) |

The block at `0x40` borrows Apple DBDMA's offsets — control at `+0x00`, command pointer at `+0x0C` — but not
its semantics: it is read back directly, writes carry no write-enable mask half, and the bits mean
something else. Offsets `0x44`, `0x48` and `0x50`–`0x58` are never accessed.

**DMA control:**

| Bit | Mask | Meaning |
|---|---|---|
| 15 | `0x8000` | run: set in every start value |
| 14 | `0x4000` | interrupt enable: must read 0 before a frame starts, is ORed in to start it, and is expected still set when the interrupt handler reads the register |
| 13 | `0x2000` | interrupt acknowledge: the handler reads the register and writes the value back with this bit set; the driver also writes it once at initialization to clear a stale interrupt |
| 12 | `0x1000` | reset: written as `0x1000`, held 20 µs, then `0` for 10 µs |
| 11, 10 | `0x0C00` | 16-bpp output, always set together when the destination has two bytes per pixel |
| 9–0 | — | never written |

The reset is performed before **every** frame (§4.5). It must leave the object cache and texture SRAM intact:
the driver re-initializes the object cache only when its mode changes and re-uploads textures only when the
texture set changes, across many resets (*inferred*). Writing `0` after the reset also clears the run and
interrupt-enable bits. What the register reads back while a frame runs, beyond bit 14, is unknown.

### 2.4 The packet port

Words written to the port are **packets** whose first word carries an opcode in bits 31–29; the remaining 29
bits are a **byte address** in the renderer's internal space [18]:

| Opcode | Header | Following words | Meaning |
|---|---|---|---|
| 1 | `0x2000_0000 \| address` | one data word (none for the strobes `0x28` and `0x2C`) | write one word |
| 2 | `0x4000_0000 \| address` | `count − 1`, then `count` data words | block write to `count` consecutive words from `address` |
| 3 | `0x6000_0000 \| …` | — | a renderable object; appears only in control programs (§2.9) |
| 0 | `0` | — | nothing (appears as padding in control blocks) |

The control program the card fetches uses the same encoding (§2.8), so the port is a host-side entry into the
renderer's one command decoder (*inferred*). The software only ever writes word-aligned addresses through the
port; it never emits opcode 2 inside a control program.

| Internal address | Contents | Section |
|---|---|---|
| `0x000000`–`0x0000FF` | renderer registers | §2.5 |
| `0x400000`–`0x41FFFF` | object cache, 128 KB | §2.6 |
| `0x800000`– | texture SRAM, one 32-bit word address per texel | §2.7 |

### 2.5 The renderer registers

Written with opcode-1 packets — by the driver directly, or by the plug-in through the driver — and, for `0x04`–
`0x07`, `0x28`, `0x2C` and `0x30`, by the control program. None is ever read back [18].

| Address | Name | Written | Value |
|---|---|---|---|
| `0x00` | Mode | every frame, per card | `(index << 8) \| flags`: index 0–3 in bits 9–8; the flags below |
| `0x04`–`0x07` | Y | control program | `(Ynext << 16) \| Ycur` for the card whose index equals the address's low two bits (§2.8) |
| `0x08` | Background | every frame | ARGB8888, the colour of uncovered pixels |
| `0x0C` | Texture configuration | every frame | `(n256 << 8) \| ((page of the first 128² map − n256) & 0xFF)`, or 0 with no maps loaded (§2.7) |
| `0x20` | Go | every frame, per card | host-physical address of the control program; the last write of a frame start |
| `0x24` | CSG equation | every frame | the CSG truth table (§3.6) |
| `0x28` | Scanline start | control program | strobe, no data: render the scanline the Y register selects |
| `0x2C` | Scanline end | control program | strobe, no data: end of the scanline group |
| `0x30` | Width − 1 | control program | output pixels per scanline, minus one |
| `0x34` | Free-list head | object-cache initialization | `0x2000`, or 0 in cache-overflow mode (§2.6) |
| `0x48`–`0x68` | Level base 0–8 | texture load | internal address of mip level *L* (256² down to 1²) of all resident maps (§2.7) |
| `0x80`–`0xBC` | Slot class 0–15 | texture load | per texture slot: 3 for a 256² map, 2 for a 128² map (§2.7) |

**Mode flags:**

| Mask | Set when | Meaning |
|---|---|---|
| `0x0008` | the destination has two bytes per pixel | 16-bpp output |
| `0x0400` | 16-bpp single-buffered destination whose base address has bit 1 set | the destination starts on an odd halfword (*inferred*) |
| `0x0800` | RAVE `kQATag_PerspectiveZ` = 1 | perspective depth: the records' depth fields hold 1/w and their texture coordinates u/w and v/w |
| `0x1000` | always | unknown |

The Y register is one register at `0x04` whose two low address bits select the card: every card reads the
same control program and latches only the write addressed to its own index (*inferred* from the encoding and
from the per-card values, §2.8). All other control-program register numbers are the same byte addresses the
port uses.

### 2.6 The object cache

The 128 KB object cache appears at internal `0x400000`. At open, after a timeout, and whenever the cache mode
changes, the driver resets the card and initializes it through the port [18]:

```
for a = 0x000000 .. 0x001FFC step 4:  write 0x400000 | a  <- 0          (the first 8 KB, cleared)
normal mode:
    register 0x34 <- 0x2000                                                (free-list head)
    for k = 0 .. 1704:  write 0x400000 | (0x2000 + 72k) <- 0x2000 + 72(k + 1)
    write 0x400000 | 0x1FF88 <- 0                                          (the 1706th node ends the list)
cache-overflow mode:
    register 0x34 <- 0
```

The cache is therefore an 8 KB reserved area and **1706 nodes of 72 bytes**, linked through their first word,
with the head in register `0x34`. 1706 is also the object budget the driver reports to the plug-in, which
switches to cache-overflow mode for a frame whose busiest scanline has more simultaneously active objects
than that (§4.7). A textured or bitmap record is exactly 72 card-relevant bytes — one link word plus 68 bytes
of header and payload (§2.9) — so each node most likely holds one active object (*inferred*). What the
reserved 8 KB holds, and what the card does in overflow mode, are open ([§6](#6-open-questions)).

### 2.7 Texture memory

**Physical organization.** The texture-storage patent's preferred embodiment [9] and the measured board [15]
agree:

- **Two banks of 131 072 words** — even texels in bank A, odd texels in bank B — so the eight texel fetches of
  a trilinear sample (§3.7) collapse to four even/odd pairs [9].
- **Shared addressing.** The layout below aligns every mip page to its own row size, so the two addresses of a
  vertically adjacent pair differ only in their low bits: `{A17:8} shared + {A7:1} × 2 banks = 24` address
  signals against 34 for two independent addresses [9]. The production board's rasterizer drives 17 address
  lines — ten shared (A1–A5, A8–A12) and seven per bank (A0, A6, A7, A13–A16) — plus per-bank chip enables and
  a shared output enable (*observed*, traced on the PCB) [15]: exactly the patent's 24 signals counting the
  per-bank lines twice.
- **16 bits per texel, 5-6-5 RGB.** The patent's embodiment moves 36 data signals, 18 bits per texel; the
  shipped card wires 16, and the two pins nearest the data bus that an 18-bit (6-6-6) texel would use are
  unconnected but show activity when textures load (*observed*) [9] [15]. The read path is two cycles of
  32 bits, about 160 MB/s at 40 MHz [15].
- **Capacity: 0x40000 texels** (512 KB) on revision-2 cards, matching the driver's figure; earlier revisions
  are given 0x10000 [18].

**Addressing as the software sees it.** The host writes texture memory through the port as one 32-bit
`0xAARRGGBB` word per texel at internal address `0x800000 + 4 · texel` — so the card packs each word to its
16-bit SRAM format on the way in (*inferred*). Texture memory is laid out **by mip level, not by map**: all
maps' level-*L* images are contiguous, a scheme the patent describes as storing "pages of the MIP maps having
the same resolution … contiguously" [9]. With `n256` 256² maps and `n128` 128² maps resident, the plug-in
programs the level-base registers as [18]:

```
base[0] = 0x800000                                   level 0: the 256² maps only
base[1] = base[0] + n256 · 256² · 4
base[L+1] = base[L] + s² · (n256 + n128) · 4          s = 128 >> (L − 1), L = 1 .. 7
```

and asserts that the total stays within the capacity. Every map has a 4-bit **page**, and a **slot**, its
position in each level array:

| Map | Pages | Slot | Levels | Slot class (`0x80 + 4 · slot`) |
|---|---|---|---|---|
| 256² | 0, 1, … upward | `page` (0 … `n256` − 1) | 9 (256² … 1²), global levels 0–8 | 3 |
| 128² | 15, 14, … downward | `page − (register 0x0C & 0xFF)` (`n256` … `n256 + n128 − 1`) | 8 (128² … 1²), global levels 1–8 | 2 |

With the plug-in's allocation, register `0x0C`'s low byte equals `16 − n128 − n256`. A texel at column *x*,
row *y* of level *L* of the map in slot *k* is at

```
address = base[L'] + (k · s² + y · s + x) · 4        s = map size >> L,  L' = L for 256² maps, L + 1 for 128²
```

**with row 0 the image's bottom row**: the driver uploads each level bottom row first (§4.8). An object record
names its map by page (§2.9); the card turns the page into a slot and a size with register `0x0C` and the
slot-class registers (*inferred*: the plug-in's formulas are exactly that inversion).

The patent's own description of the same address computation [9]: a texel address is `(x << n) | y |
offset(page, map)`, with *n* = log2 of the page's row size and `offset = M + T`, `T = MapID << 2(n − P)` for
page index *P* and *M* a per-page base from a small table — a shift-and-OR because every page is aligned to
its own size. Its parameters: u and v as 20-bit floats with 11 fraction bits; *P* as an unsigned 12-bit float
with 4 fraction bits, whose integer parts *P* and *P* + 1 are the two levels a trilinear sample blends; and a
4-bit MapID, so **at most 16 maps** are resident [9]. The shipped software's 4-bit page field (§2.9) is that
MapID. For two 256² maps the patent's offsets are:

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

— the same level-major layout the plug-in programs, here for `n256` = 2.

### 2.8 The control program

A frame is **one singly linked list in host memory** that interleaves **control blocks** and **object
records** (§2.9); the card fetches it by bus mastering from the address written to register `0x20` [18].
Every record starts with `N` link words, `N` the number of enabled cards; card *k* follows the word at offset
`4k`, and the plug-in writes the same address into all `N`. A link of 0 ends the list (*inferred*). The
record's first word after the links tells the two kinds apart: an object header carries opcode 3 in bits 31–29;
a control block's first word does not. Links are host-physical addresses; the address handed to register
`0x20` is the first link word of the first block.

A **control block** is seven command words after the links: opcode-1 writes, each followed by its data word
except the strobes, and zero words in unused slots. The plug-in builds one skeleton program per draw context
for `S = height + N` scanlines, in `ceil(S / N)` groups of `N` scanlines [18]:

| Block | Contents |
|---|---|
| initial | `Y = (Y(k) << 16) \| 0xFFFF` for each card *k* (next: its first scanline; current: none), and `0x30 = width − 1` |
| A, per group *i* | `Y` for each card, then **scanline start** |
| B, per group *i* | five zero words, **scanline end**, one zero word |

With more cards the initial and A blocks spill over two blocks, because a block holds only seven words:

| Cards | Initial block(s) | A block(s) for group *i* |
|---|---|---|
| 1 | `[Y₀, v₀, 0x30, w − 1]` | `[Y₀, v₀, start]` |
| 2 | `[Y₀, v₀, Y₁, v₁, 0x30, w − 1]` | `[Y₀, v₀, Y₁, v₁, start]` |
| 3 | `[0x30, w − 1]`, then `[Y₀, v₀, Y₁, v₁, Y₂, v₂]` | `[Y₀, v₀, Y₁, v₁, Y₂, v₂]`, then `[start]` |
| 4 | `[Y₀, v₀, Y₁, v₁, Y₂, v₂]`, then `[Y₃, v₃, 0x30, w − 1]` | `[Y₀, v₀, Y₁, v₁, Y₂, v₂]`, then `[Y₃, v₃, start]` |

(`Yₖ` is the write header `0x2000_0004 + k`, `start` is `0x2000_0028`, and block B is always five zeros,
`0x2000_002C`, zero.)

**Y values** are 13.3 fixed point at the pixel centre, `(line << 3) | 4`, with `0xFFFF` for "none". For card
*k* of *N* in group *i*, `current = N·i + k` and `next = N·(i + 1) + k`, each replaced by `0xFFFF` when not
below `S`:

```
Y_k = (next < S ? ((next << 3) | 4) << 16 : 0xFFFF0000) | (cur < S ? (cur << 3) | 4 : 0x0000FFFF)
```

For one card at 640×480 (`S` = 481) the values run `0x0004FFFF` (initial), …, `0x00AC00A4` for line 20, …,
`0x0F040EFC` for line 479, and `0xFFFF0F04` for the extra line 480: the program runs **one scanline past the
bottom of the drawable for each card**, whose next is "none" — most likely a pipeline drain, since output lags
rendering by a line (*inferred*). The low half is the scanline rendered at the following scanline-start, the
high half the same card's next scanline (*inferred*). The skeleton for 640×480 with one card is 963 control
blocks.

**Where objects go.** Objects are linked in **after block B of the group before the one in which they first
cover a pixel centre** (after the initial block for group 0), so the card meets every object just before the
scanline where it becomes active; within a group they appear in **reverse submission order**, because the
plug-in pushes each at the head of its bucket. The card builds its active list as it walks (§3.2).

### 2.9 Object records

Every object record is `N` link words, a header word and a payload, all big-endian [18]. Records are
allocated 32-byte aligned and padded to a multiple of 32 bytes. Offsets below count from the header word.

**Header:**

| Bits | Mask | Meaning |
|---|---|---|
| 31–29 | `0xE000_0000` | 3: renderable object |
| 27 | `0x0800_0000` | set on every object the plug-in emits; meaning unknown |
| 26 | `0x0400_0000` | back-facing (the RAVE triangle flag) |
| 25 | `0x0200_0000` | textured triangle |
| 24 | `0x0100_0000` | CSG operand; operand number 0–4 in bits 2–0 |
| 23 | `0x0080_0000` | bitmap tile |
| 21 | `0x0020_0000` | opaque: all three vertex alphas are 255 and the object is not a CSG operand |
| 7–4 | `0x0000_00F0` | texture page (textured triangles) |

The header base is `0x6000_0000`, or `0x6100_0000 | operand` while the RAVE CSG tag is 0–4. The other bits
are always 0.

**Gouraud triangle** (bits 25 and 23 clear):

| Offset | Format | Content |
|---|---|---|
| `+4`, `+8`, `+12` | `x << 16 \| y`, two unsigned 13.3 values | vertices 0, 1, 2, `floor(coordinate · 8)` |
| `+16`, `+20`, `+24` | IEEE single | depth of each vertex: 1/w with mode flag `0x0800`, else z — the raw float, unscaled |
| `+28`, `+32`, `+36` | A, R, G, B bytes | vertex colours, `floor(c · 255.9)` per component, unclamped |
| `+40`–`+51` | — | not written by the plug-in |

**Textured triangle** (bit 25):

| Offset | Format | Content |
|---|---|---|
| `+4`–`+24` | as above | vertices and depths |
| `+28 + 4i` | byte; nibble pair; 16 bits | vertex *i*: alpha `floor(a · 255.9)`; `(v_low4 << 4) \| u_low4`; the top 16 bits of an IEEE single holding the vertex's mip footprint (0 when the map has no mip levels) |
| `+40 + 4i` | 16 bits, 16 bits | the top 16 bits of the IEEE singles **v** and **u** |
| `+52 + 4i` | bytes 2 and 3 | diffuse `kd = floor((kd_r + kd_g + kd_b) · 42.65)`, a 1.7 fixed-point value (128 ≈ 1.0), and specular `ks = floor((ks_r + ks_g + ks_b) · 85.3)`, 0.8 fixed point |
| `+64`–`+67` | — | not written |

u and v are **20-bit floats** — sign, 8-bit exponent and 11-bit mantissa — split between the 16-bit fields and
the nibbles. They are in normalised map units (0–1 across the map). With mode flag `0x0800` they are u/w and
v/w and the card divides per pixel; otherwise the plug-in has already divided them. The footprint, used to
choose the mip level (§3.7), is:

```
perspective:  max(|∂U/∂x·q − ∂q/∂x·U|, |∂U/∂y·q − ∂q/∂y·U|, |∂V/∂x·q − ∂q/∂x·V|, |∂V/∂y·q − ∂q/∂y·V|)
              per vertex, with U = u/w, V = v/w, q = 1/w at that vertex
affine:       max(|∂u/∂x|, |∂u/∂y|, |∂v/∂x|, |∂v/∂y|), the same value in all three vertices
```

The perspective form is the texel-space derivative of (u, v) scaled by q², so the card must divide by the
interpolated q² per pixel (*inferred*). The gradients come from the float vertex positions through
`1/((y2 − y0)(x1 − x0) − (y1 − y0)(x2 − x0))`. Vertex RGB is **not** sent for textured triangles: shading is
one grey diffuse and one grey specular intensity per vertex.

**Bitmap tile** (bit 23) — one 32 × 12-pixel, one-bit tile of a RAVE bitmap:

| Offset | Format | Content |
|---|---|---|
| `+4` | IEEE single | depth |
| `+8` | `x << 16 \| y`, 13.3 | tile left and top, whole pixels (fraction 0) |
| `+12`, `+16` | ARGB8888 | the drawing colour, `floor(c · 255.99)`, twice |
| `+20 + 4r` | 32 bits | row *r* (0–11), bit 31 the leftmost pixel |

The plug-in never sets the opaque or back-facing bits on a tile, moves a partly off-screen tile fully
on-screen and zeroes the rows that fall outside, so the card always receives whole tiles inside the drawable
(and the drawable must be at least 12 rows high).

**Record lengths.** From its header to its last written word a Gouraud record is 40 bytes (52 with the
unwritten tail), a textured or bitmap record 68; with one link word that is at most 72 bytes, the
object-cache node size (§2.6). How the card decides
a record's length is not visible in the software — most likely from bits 25 and 23 (*inferred*).

**Coverage.** The plug-in buckets an object by the scanlines whose centres lie in `(yMin, yMax]` in 13.3
units — `first = (yMin + 4) >> 3`, `last = (yMax − 4) >> 3`, and none when `last < first` — so the card must
use the same vertical rule; a bitmap tile covers its twelve rows. Coordinates are never clipped by the
plug-in beyond the drawable's assertions (`x < width · 8`, `y < height · 8`).

### 2.10 The pixel-output descriptor list

The pixel-output channel consumes a list of 16-byte **big-endian** descriptors from the address in DMA
command pointer [18]:

| Word | Bits | Content |
|---|---|---|
| 0 | 31–28 | 0 |
| 0 | 27–24 | command: 0 write, 1 skip, 8 stop |
| 0 | 23–16 | 0 |
| 0 | 15–0 | byte count |
| 1 | — | host-physical destination address (write; set but ignored on skip) |
| 2, 3 | — | 0 |

The descriptors carry no source address: the source is the renderer's pixel stream (§3.9). **Write** copies
`count` bytes of the stream to the address, **skip** discards `count` bytes, **stop** ends the list. Lists are
built 32-byte aligned in physically contiguous memory, in chunks of 500 descriptors. The format resembles
DBDMA's but is not DBDMA: the command sits one nibble lower, the words are big-endian, 1 means skip and the
terminator is 8.

The plug-in builds one list per card with this algorithm [18]:

```
BuildScript(region, height, width, list, startY, yStep, clip):
    row = region.base + startY · rowBytes
    for y = startY; y < height; y += yStep:
        address = row; x = 0
        while x < width:
            if clip:                (length, inside) = the run of pixels from x on scanline y equally in or out of clip
            elif x < region.width:  (length, inside) = (width, true)
            else:                   (length, inside) = (width − region.width, false)
            emit { inside ? write : skip, length · bytesPerPixel, address }
            address += length · bytesPerPixel; x += length
        row += yStep · rowBytes
    if height mod yStep > 0 and startY >= height mod yStep:
        emit { skip, width · bytesPerPixel, row }      a padding line: this card has one scanline fewer
    emit { stop }
```

with `startY = k` and `yStep = N` for card *k*. `width` is the drawable's width rounded as the plug-in requires
and `height` its height; the card always emits `width` pixels per scanline, and pixels outside the window's
visible region or past the destination's width are discarded with skip. For a double-buffered frame the plug-in
also builds a list of the same format for the screen — logical addresses, every scanline, clipped by the
window's visible region — and interprets it on the CPU to copy the back buffer out (§4.6), advancing its
source by `count` on both write and skip and wrapping to the next source row after `width · bytesPerPixel`
bytes: the plug-in's own model of the channel.

A worked list, for two cards, 32 bpp, rowBytes 2560, a destination at `0x8100_0000`, width 640, height 481
and a 50-pixel obscured run at x = 100–149 of scanline 0 — card 0's list (start 0, step 2):

```
word 0      word 1       meaning
0000_0190   8100_0000    write 400 bytes (x 0–99)     line 0
0100_00C8   8100_0190    skip  200 bytes (x 100–149)  hidden
0000_07A8   8100_0258    write 1960 bytes (x 150–639)
0000_0A00   8100_1400    write 2560 bytes             line 2
…                                                     lines 4, 6, … 480
0800_0000   0000_0000    stop
```

Card 1 covers lines 1, 3, … 479; because 481 mod 2 = 1, it then gets one padding skip line before its stop.

### 2.11 The internal formats of the published design

The papers and patents document the rasterizer's internals in detail. None of it is visible to software —
the host speaks the interface of §2.2–§2.10 — but it is the best evidence for what the card computes, and §3
relies on it. Whether the shipped card uses these exact formats is *inferred - unverified*.

**The token interface.** The scanline-rendering patent's pipeline [8] — a vertical-interpolation stage
feeding a Z-buffer stage feeding a compositing/scanout stage, the two-chip 1992 arrangement [6] that the 1994
design integrates into one ASIC [5] — passes **tokens** between stages, each starting with a 1-bit PSetup flag
(true for set-up tokens) and a 4-bit TokenID:

| Token | TokenID | Fields |
|---|---|---|
| Pixel interpolation | 1 | X (11 bits, position in the segment), W (12, interpolation weight), A, R, G, B (10 bits each), ForceAdditive (1), 8 reserved |
| Null | 0 | a pixel interpolation token whose processing was terminated (hidden or in shadow), kept as a place-holder |
| Pixel overlay | 2 | as pixel interpolation with a 2-bit W: assigns pixel values directly |
| Z set-up | — | Z0 (32), Diffuse, Front, Z1 (32) |
| Diffuse RGB set-up | 0 | left endpoint Ad0, Rd0, Gd0, Bd0 (9 bits each) premultiplied by Kd; right endpoint the same |
| Specular RGB set-up | 3 | left Ns0 (specular power), Rs0, Gs0, Bs0 (9 bits each); right the same |
| Normal set-up | 4 | left Nx0, Ny0, Nz0 (12 bits each); right the same |
| Control | 0xF | an 8-bit OpCode, below |

| OpCode | Control token | Function |
|---|---|---|
| 1 | Load scanline DMA write register | the clear/constant value of a stage (RGB and Z target bits, 40-bit value) |
| 2 | Scanline DMA setup/start | start (11 bits) and length (11 bits) in pixels, scanout enable, scanout mode (round 40-bit pixels to 32 bits), write enable |
| 3 | Wait for scanline DMA completion | stall until the back buffer has scanned out or cleared |
| 4 | Swap buffers | swap a stage's double-buffered scanline buffers |
| 5 | Global mode setup | the mode bits below |
| 6 | Jam data | pass a 40-bit word through unprocessed |

Each scanline buffer is 648 words × 40 bits (double-buffered per stage), and the vertex parameters live in four
64 × 36-bit parameter RAMs feeding the interpolators through a crosspoint switch [8].

**Global mode** (control OpCode 5): DiffuseShade, SpecularShade, ShadowCount, ShadowTest, TransMode (1 =
additive transparency, 0 = blended), InvertShadow, Force2Visible (force the Z test visible), DisableZWrite
[8].

**The per-pixel layer store** [5]: each pixel holds **four layers** of `Z (24 bits) + ARGB (32) + state (4:
overflow flag and CSG operand state)`, double-buffered so one 16-pixel segment sorts while the previous one
composites — `(24 + 32 + 4) × 4 × 2 = 480 bits per pixel`, 7680 bits per segment. Depth, compositing and CSG
state all live there; there is no off-chip depth buffer [5] [7]. The family's depth precision varies by
document — 32-bit Z in the 1992 prototype [6], 24-bit per layer in 1994 [5], 25-bit floating point in the
successor [7] — while the shipped software sends IEEE single-precision depths (§2.9), so the card converts
them on input (*inferred*).

**Clocks** [8]: three asynchronous domains — a data clock synchronous to the object source, a pipe clock for
the rendering stages, and a scanout clock synchronous to the receiving framebuffer — with 16-word input FIFOs
and STALL/EMPTY flow control. The shipped core runs at 40 MHz [5] [6] [15].

## 3. Behaviour

### 3.1 The frame

From the card's side a frame is [18]: a reset (DMA control `0x1000` then `0`); optional packets that
initialize the object cache and upload textures; register writes (background, CSG equation, texture
configuration, mode); the descriptor-list address; DMA control set to run with interrupts enabled; and, about
100 µs later, the control program's address in register `0x20`. The card then, asynchronously:

1. walks the control program by bus mastering (§3.2);
2. renders each of its scanlines from the objects active on it (§3.3–§3.8);
3. streams every finished scanline through the descriptor list (§3.9);
4. at the list's stop, raises its interrupt if DMA control bit 14 is set (§3.10).

The software never inspects anything between steps 1 and 4, so any implementation that produces the same
pixels in the same places, then interrupts, is compatible with it.

### 3.2 Walking the control program

The card follows its own link word from record to record (§2.8). A **control block**'s command words execute
in order: a Y write is latched only by the card its address selects; `0x30` sets the output width; a
**scanline start** renders the scanline in the latched current half (none when it is `0xFFFF`); a
**scanline end** closes the group. Each **object** becomes active when the walk reaches it; since objects are
placed just before the scanline where they first cover a pixel centre, the active list is complete for every
scanline when its start strobe arrives, and an object can be retired once the rendered scanline is below its
last. The extra drain scanline renders nothing visible: the descriptor list has already ended.

The patent and paper describe what the hardware does with the stream [5] [10] [12]: the active-list
sequencer adds newly active objects and horizontally bucket-sorts them by the first 16-pixel segment they
touch, keeping the horizontal buckets in the off-chip object cache; rendering then proceeds segment by
segment — span endpoints from the object-scanline intersection, parameter interpolation at 20 million
pixels/s, the depth sort at 40 million layers/s — while the composite sequencer drains the previous segment.
Spans are generated at up to 2.5 million per second, and the state has no screen-size dependency: the design
renders "up to 8K×8K resolution in a single pass", the range of 13.3 coordinates [5]. The 16-pixel segment is a
stated trade-off against a 4×4 tile: "the other advantages of rendering in scanline order (mainly simplicity)
outweighed any potential gains" [5].

### 3.3 Rasterization

The host sends only vertices; **triangle setup is on the card** — vertex sort, active-edge test and the
vertical interpolation — by direct evaluation, without host-computed slopes [8] [18].

- **Vertical coverage**: a triangle covers the scanlines whose centres (`line + 0.5`) lie in `(yMin, yMax]`
  — the half-open rule the plug-in's bucketing assumes (§2.9), which keeps abutting edges from drawing a
  scanline twice [8].
- **Horizontal coverage**: within a scanline the card samples at pixel centres; the exact edge rule is not
  visible in the software ([§6](#6-open-questions)). The left-inclusive rule — centres from the left edge up
  to but excluding the right — is the natural counterpart of the vertical rule.
- **Interpolation**: colour (Gouraud), alpha, kd and ks, and depth vary linearly across the triangle in screen
  space — both z and 1/w are screen-linear. Texture coordinates are interpolated as u/w and v/w and divided
  by the interpolated 1/w per pixel when the mode's perspective flag is set (§3.7). Colours in the card's
  pipeline are 10 bits per component [8].
- **Points and lines** arrive as small Gouraud triangles (§4.7); there is nothing else to rasterize.

### 3.4 Visibility

Every covered pixel contributes a **layer** (fragment) to its pixel's depth-sorted list [5]:

- **Depth order**: with the perspective flag the depth field is 1/w and larger is nearer; otherwise it is z
  and smaller is nearer (*inferred* from what the plug-in sends; RAVE's Z function tag is ignored, so the
  comparison is fixed) [18].
- **Four layers per pixel**, inserted at their depth position; layers behind an **opaque** layer (header bit
  21) are dropped at insert time and never cause overflow [5]. Insertion costs 1, 1, 1.5 and 2 clocks on
  average with 0–3 layers already held, so a full pixel sorts in about 5.5 clocks against 4 to composite [5].
- **Overflow**: a pixel that needs a fifth visible layer marks its 16-pixel segment; the card composites the
  four retained layers into one, keeps it as the backmost, and **re-renders that segment's objects** to
  capture the next layers, repeating "until overflow does not occur, effectively compositing an unlimited
  number of layers from front to back" [5]. Only overflowing segments repeat: in the paper's 13 056-triangle
  test scene the rendered-triangle count falls from 26 112 (two whole passes) to 14 231. None of this is
  visible to software.
- **Equal depths**: equal-depth layers are retained, not replaced [11]; the successor renders them
  first-come-first-rendered [7], the same is *inferred* for the card. Since objects arrive per group in
  reverse submission order (§2.8), "first-come" is not the application's order.

### 3.5 Transparency and compositing

A pixel's layers are composited **front to back**, each with a single transmission coefficient [5]:

```
I  = I_front + kt_front · I_back          (per colour component; saturating at 255)
kt = kt_front · kt_back
```

with the front layer's colour already multiplied by its opacity — "premultiplied so the specular component does
not dim as transparency increases" [5]. This is RAVE's default blend, `kQABlend_PreMultiply`
(`result = source + destination · (1 − α_source)`), the only one the card offers: the plug-in ignores the RAVE
Blend tag [18] [24]. With `kt = 1 − α`, an opaque layer (α = 1) ends the composite and an α = 0 layer adds
its colour to what is behind it (*inferred*). The background register's colour shows wherever coverage is not
opaque. The Global Mode's TransMode bit (additive versus blended per frame) and the pixel token's
ForceAdditive bit exist in the published design [8]; the shipped software has no way to set them.

The advertised consequence is the card's best-known property: "hardware-accelerated per-pixel Z sorting
provides precise rendering of transparent surfaces or CSG, regardless of submission order or
interpenetration" [1] [2].

### 3.6 CSG evaluation

CSG is evaluated **inside the compositing pass, in depth order** [5]. Each pixel tracks one inside/outside bit
per operand — A′ through E′ for operands 0–4 — starting outside every operand. Walking the layers front to
back, a CSG operand's **front-facing** surface sets its bit and a **back-facing** one clears it (header bits 24,
26 and 2–0). At each crossing the card evaluates F_csg over the five bits: if F_csg changes, the crossing is a
surface of the constructed solid and the layer is rendered; otherwise it is discarded [5]. The evaluator is
"only ~500 gates"; per pixel it costs the five operand bits plus the previous F_csg [5]. CSG layers are never
marked opaque by the plug-in (§2.9), so they never hide what is behind them before the evaluation.

**The truth table** is register `0x24`, written each frame from RAVE's `kQATag_CSGEquation`, which
QuickDraw 3D's `Q3InteractiveRenderer_SetCSGEquation` sets [18] [20]: bit *s* is F_csg for state *s*, in which
operand *n* is bit *n*. QuickDraw 3D's constants fix the order — A ∩ B = `0x88888888` (bits whose index has
bits 0 and 1 set), A − B = `0x22222222`, B − A = `0x44444444` [20]. Five operands is a stated design limit
[5], and at the API level the reason CSG "is available only when a QuickDraw 3D-compatible accelerator is
present" [17].

### 3.7 Texturing

Texturing is **deferred**: a layer is textured only after hidden-surface removal and CSG have decided it is
visible, so hidden layers never touch the texture memory and textured rendering usually runs at the untextured
rate [5]. For each visible textured layer the card:

1. finds the map from the record's page (§2.7);
2. divides the interpolated u/w, v/w by 1/w when the perspective flag is set (§3.3);
3. chooses the mip level from the footprint divided by q² (perspective) or as given (affine), scaled to the
   map's size: the level is `log2(footprint · size)` (*inferred*);
4. fetches **eight texels** — four from each of the two nearest levels, in four even/odd bank pairs — and
   blends them by the fractional u, v and level: **trilinear filtering is always on**; it cannot be switched
   off, so no quality setting changes the card's speed [1] [5] [9] [15];
5. modulates the texel by the interpolated grey diffuse intensity `kd` (1.7 fixed point, up to about 2×) and
   adds the grey specular intensity `ks` — "modulation and highlighting are always turned on" [22] (the exact
   formula is *inferred*).

Texture mapping is bandwidth-limited to 10 million pixels/s [5]. Observed limits: the largest map is
256×256 (*observed* [15]); the filtering behaves as **one-dimensional along the scanline**, so a texture and its
90°-rotated twin render differently (*observed* [15]); magnified textures show noise (*observed* [15]).
Texture coordinates wrap (*inferred*). **Texel alpha** has no effect: the plug-in's resampler discards it, and no
test found a way to enable an alpha-bearing format — texturing happens after all compositing, so there is
nothing left for a texel alpha to act on [5] [15] [18]. A one-bit "black is transparent" behaviour observed once
in an overlay-like mode is the bitmap path (§3.8), not texturing.

### 3.8 Bitmap tiles

A bitmap tile draws its set bits in the tile's colour, at the tile's depth, and leaves its clear bits
uncovered; the plug-in has already dithered the source to one bit (§4.9), which is the community's "1-bit
texture where black is transparent" [15] [18]. A tile is never marked opaque; with an opaque colour it hides
what is behind it all the same.

### 3.9 Pixel output

The card produces one stream of pixels per rendered scanline, `width` (register `0x30` + 1) pixels long, in the
destination format [18]:

| Destination | DMA control | Mode | Pixel |
|---|---|---|---|
| 32 bpp | — | — | 32-bit `xRGB`, big-endian; what the unused byte holds is not known |
| 16 bpp | `0x0C00` | `0x0008` (and `0x0400` for an odd-halfword base) | 16-bit `x1555`, big-endian — the Macintosh's thousands-of-colours format (*inferred*) |

The descriptor list (§2.10) steers the stream: write spans land in the screen's VRAM (single-buffered
contexts, clipped by the window's visible region) or in a contiguous host back buffer (double-buffered contexts,
unclipped), skip spans are dropped, and stop ends the frame. Each card's stream contains only its own
scanlines. The published design composites in 40-bit pixels and rounds to 32 bits on output [8].

At the system level the output is real PCI traffic in small bursts: the observed card moves **16-pixel tokens,
one at a time**, to the framebuffer [15] — the rendering segment of §3.2. This is why a card renders 10–15%
faster in the framebuffer's bus segment (§1.5), and why three cards cannot fully use a 33 MHz 32-bit PCI bus:
"the 33MHz 32bit PCI-bus is not capable managing the amount of small DMA transfers needed to fully use the
potential" [15]. Because the card emits finished scanlines top to bottom, single-buffered rendering does not
tear [22].

### 3.10 Completion, interrupts and the watchdog

The card raises **one interrupt per frame, at the end of its descriptor list**, gated by DMA control bit 14
[18]; there is no other interrupt source and no status register beyond DMA control. The design intent is the
same: the pipeline is to "interrupt the processor only when it has finished processing a frame" [7]. The
driver's handler reads DMA control, writes it back with bit 13 set to acknowledge, warns if bit 14 is clear,
and clears that card's running flag; when no enabled card is still running it releases the card and sends
the plug-in `{1 (done), render time}` (§4.4).

The driver marks each card running **after** its 100 µs delay and **before** writing register `0x20`, and only
the interrupt handler clears the flag: a completion signalled synchronously with the `0x20` write would arrive
before the flag is set and be taken as spurious (*inferred*).

**Watchdog.** Each frame arms a timer for `max(3 s, 10 × the longest render so far)`. If it fires, the driver
counts a timeout, resets every card, re-initializes the object cache, and sends the plug-in `{2 (timeout),
elapsed}`; the plug-in then drops the frame (it skips the back-buffer copy) [18].

### 3.11 Reset

**DMA control bit 12** resets the channel and the renderer's command state: it stops any frame, drops a pending
interrupt and returns the packet parser to expecting a header (*inferred*). The driver issues it before every
frame and around initialization; it must not clear the object cache or texture SRAM, whose contents the driver
relies on across resets (§2.3). After the reset release the driver writes DMA control once with bit 13 at
initialization, to clear any interrupt latched before. PCI RST# returns the configuration header to its
power-on state; the software re-initializes everything it uses after that (§4.3).

### 3.12 Multiple cards

Up to **four** cards cooperate on one frame by **interleaved scanlines** [18]: each card learns its index and the
card count from configuration register `0x40` and the mode register's index field, receives the **same**
control program, latches only its own Y register writes (§2.8), and owns a descriptor list covering scanlines
`k, k + N, k + 2N, …` (§2.10). Every card walks every object and skips those not active on its scanlines; the
per-card link words exist in every record although the plug-in fills them identically. Texture uploads and
register writes are broadcast to every enabled card, so texture memory is replicated, not pooled. The frame is
done when every card has interrupted.

The driver registers up to five cards and enables at most four; the RAVE engine registers once, so to an
application several cards look like one faster engine. This is the design family's intended scaling — several
scanline renderers fed from one active list [10] [12], and in the successor up to four rasterizers behind one
I/O ASIC [7]. Observed scaling with two cards is about 80% of an ideal doubling, 1.2× to 1.8× depending on the
test (§4.12) [15]; since the split is by scanline, the repeated per-object setup and the shared PCI bus are the
likely limits rather than a partition imbalance (*inferred*).

### 3.13 Timing and performance

| Quantity | Value |
|---|---|
| Core clock | 40 MHz [5] [15] |
| Untextured fill | 20 million pixels/s [1] [5] |
| Textured fill | 10 million trilinear pixels/s [1] [5] |
| Depth sort | 40 million layers/s [5] |
| Span generation | 2.5 million spans/s [5] |
| Triangles | 120 000/s [1] |
| Measured output | about 11 million pixels/s and 10 million texels/s on a 7500 [15] |

Software checks no timing beyond the watchdog's three seconds (§3.10). The driver measures each frame's
render time from the go write to the last interrupt and reports it to the plug-in. Overclocking [15]:

| Configuration | Result |
|---|---|
| 40 MHz (stock oscillator) | normal operation; runs hot in a full enclosure |
| 44 MHz | stable |
| 45 MHz | freezes within about ten minutes |
| 54 MHz (replaced oscillator; 12 ns object-cache SRAM; added cooling) | stable, heat-limited — "a passive cooler is needed" |

Apple's specification quotes only ambient limits (10–40 °C operating) [3]; no power or thermal figure for the
card itself is published.

## 4. Programming model

### 4.1 The software stack

```
application
  └─ QuickDraw 3D (interactive renderer)          enables the hardware engine
       └─ RAVE manager                            loads engines; draw contexts
            └─ Apple QD3D HW Plug-In              the "Apple PCI Hardware" engine: builds frames
                 └─ .GothamDriver (Apple QD3D HW Driver)   Control/Read calls; owns the cards
                      └─ the card(s)
```

QuickDraw 3D is a high-level, cross-platform 3D API with retained objects and the 3DMF file format; its
interactive renderer selects hardware acceleration without application involvement [16] [17]. Below it, RAVE
("Tinsel Town" and "Drive3D" internally, and in 1.0.3 the "QuickDraw™ 3D Accelerator" library) is the
rasterizer layer with pluggable engines [23] [24]. The plug-in is a RAVE engine built on Apple's sample engine
skeleton [24]. The driver is a native PCI driver that does nothing but move bytes to the cards; all geometry
handling, frame building and texture management is in the plug-in [18].

**The enable handshake.** The plug-in registers its engine and immediately calls `QAEngineDisable(0, −1)`
(vendor Apple, engine `kQAEngine_AppleHW`). The RAVE manager's engine enumeration skips disabled engines, so an
application sees the card only after calling `QAEngineEnable(0, −1)` — which QuickDraw 3D does before choosing
an engine, from 1.0.3 on, and which Apple's developer Q&A tells RAVE developers to do [18] [22] [23]. Most RAVE
games never did (§4.11). Even when enabled, the engine lacks features many games select on (§4.6).

The QuickDraw 3D stack changed quickly around the card: the April 1995 beta (1.0b1c5) had no accelerator
interface at all, and 1.0.3 shipped with the card in October 1995 [15].

### 4.2 Installation and discovery

**Files** [1] [18]:

| File | Kind | Role |
|---|---|---|
| Apple QD3D HW Driver | native driver (`ndrv`), unit name `.GothamDriver` | matched to `pci106b,9` by name; one instance per card |
| Apple QD3D HW Plug-In | shared library, type `shlb`, creator `tnsl` (*inferred* from the loader) | the RAVE engine |

Both live in the Extensions folder. The card is disabled by moving **only the plug-in** out and restarting
the QuickDraw 3D applications; the driver may stay loaded [1]. The RAVE manager loads every `shlb`/`tnsl` file
from the application's folder and the Extensions folder; a plug-in whose initialization fails — no driver, no
card — unloads itself and registers nothing [23].

**The driver's description** (`TheDriverDescription`, identical in every build) [18] [21]:

| Field | Value |
|---|---|
| Signature, version | `'mtej'`, 0 |
| Match name | `pci106b,9` |
| Driver version | 1.0a6 |
| Runtime flags | loaded upon discovery, opened upon load |
| Driver name | `.GothamDriver` |
| Service | category `'ndrv'`, type `'genr'` (generic: not a display or block driver), version 1.0a6 |

**Name Registry properties** the driver uses [18] [21]:

| Property | Requirement | Use |
|---|---|---|
| `assigned-addresses` | exactly 20 bytes, one memory-space entry | `phys.lo` is the card's register base, used directly as a logical address (PCI memory is mapped 1:1) |
| `driver-ist` | 24 bytes (an interrupt set of three members) | member 0, the chip interrupt source, receives the interrupt handler; without it: "No driver-ist property; are your ROMs up-to-date?" |
| `gothamSharedGlobals` | 4 bytes | created by the first card's driver instance, holding a pointer to a block that every instance shares; the multi-card bookkeeping lives there |

The driver reads no other property (`reg`, `AAPL,address`, `AAPL,interrupts`, `name` are unused) and makes no
mapping or DMA-preparation call; the plug-in prepares all memory the card touches.

### 4.3 Driver initialization

The driver's `DoDriverIO` handles the standard native-driver commands [18] [21]:

| Command | Behaviour |
|---|---|
| Initialize | per card, below |
| Open | lock, initialize the object cache (normal mode), unlock |
| Close | wait for any frame in flight |
| Control | the plug-in's "set" calls (§4.4) |
| Read | the plug-in's "get" calls (§4.4) |
| Write, KillIO, Finalize, Replace, Superseded | no effect |
| Status | `statusErr` |

**Initialize**, per card:

1. Find or create the shared global block (`gothamSharedGlobals`).
2. Register the card: at most five; the card is enabled if fewer than four are.
3. Read `assigned-addresses` (§4.2) for the register base.
4. Configuration accesses 1–5 of §2.2: position `← 0`, check the ID, read the revision (warning "White Magic
   cards have different revs!" if it differs from the others'), `0x0C ← 0xFF08`, command `← 0x16`.
5. Recount the enabled cards and write every enabled card's position register (§2.2).
6. Lock (which resets every enabled card, §4.4), reset again, unlock.
7. DMA control `← DMA control | 0x2000`: clear any stale interrupt.
8. Install the interrupt handler on `driver-ist` member 0, keeping the existing enabler, and enable the
   interrupt.

Nothing is torn down at finalization.

### 4.4 The driver/plug-in protocol

The plug-in opens `.GothamDriver` and talks to it with synchronous Control and Read calls [18]. Every Control
call except Lock and SetCardEnables requires the lock; operations on the card are **broadcast** to every
enabled card.

**Control** (`csCode`, parameters in `csParam[0..4]`):

| csCode | Call | Parameters | Effect |
|---|---|---|---|
| 1001 | Lock | 109 (interface version), context ID, overflow flag | wait until no frame runs and the card is unlocked; lock; reset every card; re-initialize the object cache if the cache mode changed (overflow flag set → overflow mode); note whether the context changed |
| 1002 | StartRender | control-program address, mode word, software-interrupt ID | the frame start of §4.5 |
| 1003 | SetGothamRegister | address, value | one opcode-1 packet to every card |
| 1004 | SetDBDMAControlProgram | descriptor-list address for each enabled card (up to four), DMA control word | stored for StartRender; the number of lists must equal the number of enabled cards |
| 1005 | SetDBDMARegister | offset, value | write `0x40 + offset` on every card (unused by the plug-in) |
| 1006 | WriteMapPage | internal address, *n*, source, source row bytes | upload an *n* × *n* block of words, rows bottom-up (§4.8) |
| 1010 | SetCardEnables | bit mask | enable registered cards, rewrite the position registers, lock, re-initialize, unlock |

**Read** (selector in `ioMisc`):

| Selector | Call | Returns |
|---|---|---|
| 1007 | GetGeneralInfo | the 100-byte structure below |
| 1008 | IssueContextID | a new context ID |
| 1009 | GetContextHasChanged | whether another context used the cards since this one's last lock — if so, the plug-in reloads its textures |
| 1011 | GetDBDMARegister | card 0's DMA control (1.5 and later; unused by the plug-in) |

**GeneralInfo:**

| Offset | Content |
|---|---|
| `0x00` | 109, the interface version (the plug-in checks it) |
| `0x04` | enabled cards |
| `0x08` | registered cards |
| `0x0C` | the lowest Revision ID among the cards |
| `0x10` | timeouts so far |
| `0x14` | the last context ID |
| `0x18` | texture capacity in texels: `0x40000` when the lowest revision is 2, else `0x10000` |
| `0x1C` | each card's register base (5 entries) |
| `0x30` | each card's revision (5) |
| `0x44` | each card's enable (5) |
| `0x58` | object-cache nodes, 1706 |
| `0x5C` | the last render time, seconds (float) |
| `0x60` | the longest render time, seconds (float) |

**Completion** reaches the plug-in as a software interrupt it created at open: the driver passes
`{long code; float seconds}`, code 1 for a finished frame and 2 for a timeout.

### 4.5 The frame sequence

At `QARenderEnd` the plug-in starts the frame it has built, after waiting for the previous one to finish [18]:

1. **Lock** (1001), then GetContextHasChanged (1009). The lock resets every card: DMA control `0x1000`, 20 µs,
   `0`, 10 µs, and re-initializes the object cache if the overflow mode changed.
2. **Textures**: if the texture set changed, upload every resident map and write the level bases and slot
   classes (§4.8).
3. **Descriptor lists** (1004) with DMA control `0x8000`, or `0x8C00` for a 16-bpp destination.
4. **Registers** (1003): background `0x08`, CSG equation `0x24`, texture configuration `0x0C`.
5. **StartRender** (1002) with the control program's physical address and the mode word
   (`flags | 0x1000 | perspective · 0x0800`). The driver arms the watchdog, then for each enabled card *n*:
   mode register `0x00 ← (n << 8) | mode`; DMA command pointer `←` card *n*'s list; checks DMA control bit
   14 is clear; DMA control `← control | 0x4000`. Then it waits 100 µs, marks every card running, and writes
   register `0x20 ←` the program address on every card.

Before step 5 the plug-in flushes the processor's data cache over the program, the objects and the lists, and
for a single-buffered context protects the cursor's area of the screen. Completion (§3.10) runs the plug-in's
completion function: unprotect the cursor (single-buffered) or copy the back buffer to the screen (§4.6), then
call the application's render-completion notice.

### 4.6 The RAVE engine

**Registration** (at load) [18] [23]: open the driver and create the completion software interrupt; get a
context ID; read GeneralInfo (copying the texture capacity and the node count); register a Display Manager
notifier (1.5 and later); register the engine; disable it (§4.1).

**Engine methods**: draw-context new/delete, device check, gestalt, texture new/detach/delete, bitmap
new/detach/delete, and (1.5 and later) colour-table new/delete and texture/bitmap colour-table binding. The
RAVE 1.6 access methods are not provided.

**Gestalt** (the same since 1.0.3 except as noted):

| Selector | Answer |
|---|---|
| Optional features | `0xCB7`: deep Z, texture, high-quality texture, blend, alpha blend, Z-sorted, no-clear, CSG — not texture colour, antialiasing, perspective Z, CL4/CL8, bound-to-device |
| Fast features | `0x5E`: Gouraud, texture, high-quality texture, blend, Z-sorted — not line |
| Vendor, engine | 0 (Apple), −1 (`kQAEngine_AppleHW`) |
| Revision | 0 in 1.0.3, 1 from 1.5 |
| Name | "Apple PCI Hardware" (18 characters) |
| Texture memory, fast texture memory | free texels (1.5 and later): `capacity − n128 · 0x5555 − n256 · 0x15555` |
| anything else | unknown |

**Device check**: at least one enabled card; with a lowest revision of 2, a 16- or 32-bpp GDevice or an ARGB16
or ARGB32 memory device; otherwise 32 bpp only. Any monitor or display card is accepted — the card writes to
any framebuffer.

**Draw contexts.** A context allocates an object heap and two control-program heaps (ping-pong between frames;
one for a cache context), in physically contiguous chunks it grows on demand, then builds the skeleton program
(§2.8) and the descriptor lists (§2.10) for its destination. Every context is deep-Z. It is
**double-buffered** — the card writes a contiguous host back buffer, copied to the screen afterwards by the CPU
with the plug-in's software model of the descriptor list — when the application asks, when it gives a clip
region, or, while virtual memory leaves the plug-in's buffers physically discontiguous, when the destination is
a memory device or an offscreen GDevice; otherwise it is **single-buffered** and the card writes the screen
directly. Destination changes after a display-mode switch are only forwarded to the
application as a notice; the context keeps its old layout.

**Draw methods**: point, line, Gouraud and textured triangle, bitmap, render start/end/abort, flush, sync, and
(1.5 and later) the notice methods. The vertex-array and triangle-mesh methods are left to the RAVE manager,
which decomposes them into single triangles.

| Call | Behaviour |
|---|---|
| RenderStart | clears the frame; packs the background colour; with an initial (cache) context, copies its objects into this frame — a cache context caches geometry, not pixels; the dirty rectangle is ignored |
| RenderEnd | starts the frame (§4.5) and returns at once; a non-NULL modified rectangle is not supported |
| Flush | with virtual memory off, renders what has accumulated and resets the objects' links, so RenderEnd renders everything again; with it on, nothing |
| Sync | waits for the frame in flight |
| RenderAbort | always "not supported" |

**State tags**: only the background colour, perspective Z, the texture, the CSG tag and the CSG equation have
any effect; Z function, width, antialiasing, blend mode, texture filter and texture operation are stored and
ignored; tags above 15 are not supported. Notices: render completion and display-mode change are supported;
buffer initialize and buffer composite are refused. From 1.5.4 the plug-in registers a QuickDraw
draw-notification over the destination, so QuickDraw drawing there waits for a pending frame.

### 4.7 Building a frame

The plug-in turns each RAVE primitive into object records (§2.9) and pushes each at the head of the bucket for
its first scanline, the control block before that scanline's group (§2.8) [18]:

| Primitive | Records |
|---|---|
| Gouraud triangle | one Gouraud record; the opaque bit when all three alphas are 255 and no CSG tag is set |
| Textured triangle | one textured record with the map's page; marks the map touched |
| Point | one Gouraud triangle `(x + 0.5, y), (x, y + 0.99), (x + 0.99, y + 0.99)` at the integer pixel |
| Line | two Gouraud triangles forming a one-pixel-wide quad, offset ±0.5 pixel across the major axis; endpoints clamped to `[0.5, width − 0.55]` × `[0.5, height − 0.55]`; the width tag is ignored |
| Bitmap | one tile per 32 × 12 pixels (§2.9, §4.9) |

A per-scanline histogram of objects starting and ending decides **cache-overflow mode**: walking the scanline
groups, the plug-in adds the objects that start and subtracts those that end, and if the running total ever
exceeds the node count (1706) it asks the driver for overflow mode (§2.6). After a frame starts, the plug-in
switches to the other program and heap and resets the links of the one just finished from the shadow words it
keeps (host-only words after each record's payload). If a heap allocation ever fails, the context drops every
later primitive and keeps returning out-of-memory.

### 4.8 Textures

**Maps.** Every texture becomes a square **128² map with 8 levels** or **256² map with 9 levels**: a source whose
larger side is at most 128 becomes a 128² map, anything larger a 256² map (larger sources are reduced). A source
that is not exactly that size, square, 32-bit and mipmapped is **resampled in software**: box-averaged when
reducing, interpolated from a 3×3 neighbourhood when enlarging; non-mipmapped sources get a full box-filtered
chain. The resampler keeps only R, G and B — **alpha becomes 0**; only an exact-size mipmapped ARGB32 texture
reaches the card with its alpha byte. RGB16 sources are expanded `R << 19 | G << 11 | B << 3` first. Colour-table
textures (1.5 and later) are expanded through the table, whose entries get alpha `0xFF` except an optional
transparent index 0, which becomes `0x00000000`. 1.0.3 accepts only mipmapped RGB32 [18].

**Allocation** in texels, against the capacity from GeneralInfo: a 128² map with its chain costs `0x5555`
(21 845), a 256² map `0x15555` (87 381). With 0x40000 that is **twelve 128² maps or three 256² maps** per frame,
the limit Apple's Q&As state [22]. 256² maps take pages 0, 1, … upward, 128² maps pages 15, 14, … downward.
When a 128² map does not fit, the most recent 256² map is **demoted** to the 128² list (its top level dropped);
when a 256² map does not fit it falls back to a 128² slot; at each frame start demoted maps are **promoted**
back while space allows. Maps that move between pages have the page fields of the frame's textured records
rewritten. A deleted map is freed at the end of the frame and the remaining pages are renumbered compactly.

**Upload**: when the texture set changes, or another context used the card, every map is uploaded through
WriteMapPage (1006): for each level, rows bottom-up, one opcode-2 packet per row, to
`base[L] + slot · s² · 4` (§2.7), broadcast to every card. A full 512 KB reload is 262 144 port writes per
card. A demoted 256² map uploads levels 1–7 only; its 1×1 level is not written. Then the plug-in writes the
level bases and the slot classes.

### 4.9 Bitmaps

Every bitmap is converted to **one bit per pixel**: an Alpha1 bitmap is used as it is (1 = drawn); RGB16/32
and colour-table bitmaps are drawn with QuickDraw's dithering copy into a 1-bpp bitmap and inverted, so bright
source pixels are drawn and dark ones transparent [18]. Drawing a bitmap emits one tile per 32 × 12 pixels in
the single vertex colour, at the vertex's depth, with truncated integer coordinates (§2.9). The bitmap scale
tags are not supported.

### 4.10 What the host does in software

The division of labour leaves the host everything ahead of rasterization [5] [15]: transform, lighting and
clipping; the conversion to records and the Y bucket sort; texture preparation; and, for double-buffered
contexts, the final copy. A faster CPU measurably raises the card's throughput in complex scenes, while the
card's own fill rate dominates simple ones (*observed* [15]). Whole-scene deferral — nothing renders before
RenderEnd — and the many megabytes of display list in host memory are what Apple's Q&A warns RAVE developers
about [22].

### 4.11 Observed run-time behaviour

The card's engine descriptor, probed on live hardware with a QuickDraw 3D 1.6 SDK sample [15]:

| Field | Value |
|---|---|
| ASCIIName | "Apple PCI Hardware" |
| VendorID | 0 |
| EngineID | −1 |
| Revision | 1 |
| FastFeatures | Gouraud, Texture, TextureHQ, Blend, ZSorted |
| OptionalFeatures | DeepZ, Texture, TextureHQ, Blend, BlendAlpha, ZSorted, NoClear |

— the plug-in's gestalt (§4.6); the probe omits CSG, which the plug-in reports. A same-machine RAVE card for
comparison (a Formac/S3 ViRGE) reported VendorID 1716347235, EngineID 2, FastFeatures Line/Gouraud/Texture/
TextureHQ [15].

Application behaviour on Power Macintosh 7500/8500-class machines under Mac OS 8.6 and QuickDraw 3D 1.6 [15]:

- Works fully: **Havoc**, **Virtual Wings** 1.0.2, **Virtual Chess** (the last with slight render differences
  against the software renderer).
- Works with texture errors (missing textures, the texture store overflowing): **Nanosaur**, **Weekend
  Warrior**, **Bugdom**.
- Not recognized or fails: the RAVE titles — Tomb Raider 1/2, VR Soccer, MechWarrior 2, NASCAR Racing,
  IndyCar Racing, HotCarts, Descent 1/2, Unreal; the Quake RAVE patch freezes with a type-10 error; Quake II
  is OpenGL-only. This is §4.1's enable handshake made visible.

### 4.12 Observed performance

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

The geometry rates sit at and above the 120 000 triangles/s specification for small triangles and fall to
about a tenth of it for screen-filling ones — consistent with the 20/10 million pixels/s fill limits, and with
deferred texturing paying off on large triangles: at 1000 textured pixels the card leaves the ViRGE, which
textures before depth, behind by 2× [5] [15]. Two cards land in the region of a basic ATI Rage Pro or a
first-generation 3Dfx Voodoo Graphics board, against which a single card is 2–4× slower in raw tests [15]
(those cards' own pages are [rage-128.md](rage-128.md) §1 and [voodoo1.md](voodoo1.md) §1). On a 9500 with a
375 MHz G3 upgrade, Gerbils! runs at 40–50 fps on one card and 70–85 fps on two [15].

## 5. Quirks & errata

1. **The engine disables itself.** RAVE applications never select the card unless they call
   `QAEngineEnable(0, −1)` (§4.1) [18] [22].
2. **Sixteen-pixel bands at depth ties.** Where two surfaces' depths cannot be resolved, the card draws a
   visible 16-pixel-wide pattern where they overlap (*observed* [15]) — the rendering segment showing through the
   per-segment sort. The family treats depth equality as exact [7] [11].
3. **One-dimensional trilinear filtering.** Identically textured geometry renders differently at different
   orientations (*observed* [15]; §3.7).
4. **Upscaling noise.** Magnified textures show noise (*observed* [15]).
5. **Missing textures under store pressure.** Applications that exceed the texture budget render with missing
   textures (*observed* [15]); demotion only goes so far (§4.8).
6. **Hard-wired texture capacity.** The capacity comes from the revision, not the memory fitted: doubling the
   texture SRAM to 2 MB changes nothing (*observed* [15]; §4.4).
7. **No texture alpha.** The resampler zeroes alpha and nothing composites a texel's alpha (§3.7, §4.8).
8. **Colour-depth gate.** No acceleration at 256 colours or fewer, and none at thousands of colours on a card
   below revision 2 (§1.5, §4.6).
9. **Unsupported RAVE calls.** `RenderAbort` and `RenderEnd` with a modified rectangle are not supported; the
   buffer-initialize and buffer-composite notices are refused (§4.6).
10. **Sticky out-of-memory.** After one failed allocation a draw context never renders again (§4.7).
11. **Display-mode changes are not followed.** A context keeps its old destination layout after a mode switch;
    the application must recreate it (§4.6).
12. **A demoted map loses its 1×1 level** on upload (§4.8).
13. **Equal-depth order is not submission order.** Objects reach the card per scanline group in reverse
    submission order (§2.8, §3.4).
14. **Flush is unavailable with virtual memory**, and Apple advised turning virtual memory off (§4.6) [1].
15. **Runs hot.** Stock cards need airflow in a full enclosure, and heat limits overclocking before the SRAM
    does (§3.13) [15].
16. **Reserved token bits** in the published design are policed by convention only [8].

## 6. Open questions

1. **PCI details.** The class code, the BAR size, and whether the `0xF` nibble of configuration register `0x40`
   means anything; what a disabled card (position 0) does with the port and the DMA channel (§2.2).
2. **DMA control read-back.** What the register reads while a frame runs, beyond bit 14; whether bits 11 and 10
   act separately; whether the channel writes status into descriptor words 2–3; the descriptor bits the
   plug-in always clears (§2.3, §2.10).
3. **Mode flags `0x1000` and `0x0400`** beyond the conditions under which they are set (§2.5).
4. **Object records.** Header bit 27; how the card determines a record's length; whether it reads the unwritten
   tail of a Gouraud record; whether it rewrites link words while rendering — the plug-in resets every object's
   links after a partial flush, which would only matter if it did (§2.9, §4.7).
5. **The object cache.** What the reserved first 8 KB holds, the node format, and what the card does in
   cache-overflow mode (§2.6).
6. **Pixel-exact rules.** The horizontal edge rule; colour interpolation and rounding; the depth comparison's
   precision and tie-breaking; the texture modulation formula behind the 1.7/0.8 kd/ks encodings; the mip
   level selection and bias; texture-coordinate wrapping; the unused byte of a 32-bit output pixel (§3.3–§3.9).
7. **Compositing of non-opaque layers.** Whether the card weighs a layer's colour as RAVE's premultiplied blend
   (§3.5) or by its alpha; what the Global Mode's TransMode is set to on the shipped card.
8. **The companion chip.** The second ASIC's identity and its division of labour with the rasterizer (§1.2).
9. **Token-format fidelity.** Whether the shipped card's internals match the patent's tokens and the paper's
   layer store (§2.11).
10. **Depth format.** 24-bit stored Z, 32-bit endpoints or a floating-point format, and how the IEEE single
    depths are converted (§2.11).
11. **The multi-card bottleneck.** What limits two cards to ~80% of a doubling (§3.12).
12. **The 343S1167 marking** is community-reported, not photographically confirmed, and neither ASIC carries a
    public Apple part number (§1.2).
13. **Texture data width.** The two unconnected texture-data pins with load-time activity, and the SRAM parts'
    speed grades (§2.7).
14. **Seed-card versus production differences.** The spring-1995 pre-release board supported three textures
    against the production twelve, and the production record reports both "3 or 12" — silicon revision,
    driver or memory (§2.7).
15. **CSG exposure.** Which applications ever exercised CSG on the card (§3.6).
16. **Power and thermal figures** for the card itself (§3.13).
17. **The unshipped successor.** Whether the 1997 design was intended as a Gotham replacement product, and how
    far silicon travelled [7] [14].

## Appendix A. Granny Smith implementation notes

The emulator models the card as `pci_card="qd3d"`; its design is described in
[`qd3d.md`](../../../../internals/core/peripherals/pci/cards/qd3d.md). Where §6 leaves a choice open, the
model picks: pixel-centre sampling with a left-inclusive horizontal rule; screen-linear colour and depth;
eight layers per pixel without segment overflow; premultiplied compositing over the background; the CSG walk
of §3.6; 5-6-5 texel storage; trilinear filtering with the level `log2(footprint · size)`; `x1555` 16-bit
output; and a completion interrupt delivered after a fill-rate-based delay, never synchronously with the go
write.

A rig that exercises the whole stack: a Power Macintosh 8500 with Mac OS 7.6, QuickDraw 3D 1.6 (the release
extensions: under emulation the 1.5.4 debug QuickDraw 3D libraries stop at an unimplemented trap without a
debugger installed) and the 1.6 driver and plug-in. The software that checks the card end to end is all Apple's: RaveEngineInfo (the engine list,
which shows "Apple PCI Hardware" first with 0x40000 texels when the card works), Gerbils! (Apple's acceptance
test for the card), TextureEyes, the QuickDraw 3D Viewer in SimpleText with 3DMF models, and Sneak3View,
which draws the same scenes through two engines side by side — the most direct comparison of the card against
Apple's software engine [19].

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
    continuity tracing of the rasterizer ASIC's SRAM interface (the address/data/enable pin map of §2.7),
    overclocking experiments (§3.13), single- and dual-card benchmarking on Power Macintosh 7500/604e-200
    and 9500/G3-375 hardware under QuickDraw 3D 1.6 and Mac OS 8.6 (§4.11, §4.12, including the Sneak3view
    engine-descriptor probe and the comparative benchmark document against the Matrox Millennium II and S3
    ViRGE), application compatibility testing (§4.11), texture-format experiments with a modified texture
    test program, and the development-history reconstruction of §1.4 (the White Magic/Gotham/seed-card
    lineage, the 343S1167 marking report and the QD3D 1.0b1c5/1.0.3 software history).
16. Apple Computer, Inc., *QuickDraw 3D Preliminary User Interface Guidelines*, June 1995 — QuickDraw 3D's
    design context: the user-centred interaction model, the viewer/interactive-renderer concept and the
    3D-application user classes the card was to serve.
17. "Programming 3D with QuickDraw 3D", *MacTech Magazine* — the QuickDraw 3D programming model as
    documented for developers: the plug-in rendering architecture, transparent accelerator access
    ("no changes to your code are needed"), the interactive software renderer's performance envelope, and
    the rule that CSG support exists "only when a QuickDraw 3D-compatible accelerator is present".
18. Apple Computer, Inc., *Apple QD3D HW Driver* (native driver `.GothamDriver`) and *Apple QD3D HW
    Plug-In* — the 1.0.3 retail builds on the card's CD, the 1.5d22 builds in the RAVE 1.5 SDK, the 1.5.4
    debug builds in the QuickDraw 3D 1.5.4 SDK, and the 1.6 builds in the QuickDraw 3D RAVE 1.6 DDK and
    QuickDraw 3D 1.6; PowerPC disassembly, with function names from the debug builds' traceback tables
    (`DriverInitializeCmd`, `GrPCIDoIt_StartRender`, `GrPCIDoIt_InitializeGothamAll`,
    `GmInterruptServiceRoutine`, `ZGrRegister`, `ZGrDrawPrivateNew`, `ZGrControlProgram_Build`,
    `ZGrDrawTriGouraud`, `ZGrDrawTriTexture`, `ZGrDrawBitmap`, `ZgrDBDMAProgram_BuildScript`,
    `ZGrDBDMA_Engine`, `ZGrTextureLoad`, `ZGiTexture_InitMapPageBase`, `ZGrEngineGestalt` and others).
19. Apple Computer, Inc., *RaveEngineInfo* and *Sneak3View* — sample programs of the QuickDraw 3D 1.6 SDK
    and the QuickDraw 3D RAVE 1.6 DDK: an engine lister that prints every registered RAVE engine's gestalt
    answers, and a test harness that draws the same scenes through two engines side by side.
20. Apple Computer, Inc., *QuickDraw 3D* SDK interfaces, `QD3DRenderer.h` (QuickDraw 3D 1.1 and later) — the
    `TQ3CSGEquation` constants (`kQ3CSGEquationAandB = 0x88888888`, `kQ3CSGEquationAandnotB = 0x22222222`,
    `kQ3CSGEquationnotAandB = 0x44444444`) and `Q3InteractiveRenderer_SetCSGEquation`, whose value the
    plug-in hands to register `0x24` unchanged.
21. Apple Computer, Inc., *Designing PCI Cards and Drivers for Power Macintosh Computers*, revised edition
    (1999) — the native driver model: `TheDriverDescription`, the `DoDriverIO` command codes, the Name
    Registry, the `assigned-addresses` and `driver-ist` properties, and the Driver Services timing and
    interrupt calls.
22. Apple Computer, Inc., Developer Technical Support, *QuickDraw 3D Q&A* QD3D 31, 34, 59, 61 and 91 — the
    texture budget (twelve 128×128 or three 256×256 32-bit textures per frame), the explicit
    `QAEngineEnable` the hardware engine needs, deep-Z-only contexts, single-buffered rendering without
    tearing, always-on modulation and highlighting, whole-scene buffering in host memory, and the PCI-only
    `DriverServicesLib` dependency.
23. Apple Computer, Inc., *QuickDraw 3D* 1.0.3, 1.5.4 and 1.6 system extensions — QuickDraw™ 3D, QuickDraw™
    3D RAVE, QuickDraw™ 3D IR and QuickDraw™ 3D Accelerator 1.0.3; PowerPC disassembly: the engine loader
    (`shlb`/`tnsl` files from the application and Extensions folders), the engine enable flag the
    enumeration tests, and the `QAEngineEnable(0, −1)` calls in QuickDraw 3D's engine choice.
24. Apple Computer, Inc., *QuickDraw 3D RAVE* 1.6 DDK — `RAVE.h` (engine and draw-method tags, gestalt
    selectors and feature bits, the blend modes and state tags) and the `EmptyEngine` sample engine
    (`TtTinselTown.c`), the skeleton the plug-in is built on.
