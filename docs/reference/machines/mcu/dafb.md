# DAFB — the Quadra 700/900/950 built-in video controller

**Contents:**

1. [Overview](#1-overview) — what the part is, which machines carry it, the four blocks behind one aperture
2. [Register file](#2-register-file) — the slot-$9 apertures; core registers, the Swatch CRTC, the AC842/AC842a RAMDAC, the DP8531 clock generator
3. [Behaviour](#3-behaviour) — address decode and VRAM, pixel formats, scan timing, interrupts, monitor sense, TurboSCSI, block write
4. [Programming model](#4-programming-model) — the ROM's video driver, the observed boot-time init sequence, mode and depth changes, palette loading
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 What the part is

DAFB — the **D**irect **A**ccess **F**rame **B**uffer — is the custom IC that controls the built-in
video of the first-generation 68040 Quadras. Apple's own description: *"DAFB (Direct Access Frame
Buffer), an IC that connects directly to the system bus and controls the video RAM (frame buffer)"*
[1] p. 6; the block diagram chapter lists it as one of the four controller ICs on the 25 MHz system
bus, alongside the memory controller, the NuBus controller and the SCSI interface [1] p. 4.

Unlike the RAM-based video (RBV) parts of the IIci generation, DAFB does not scan a frame buffer out
of main memory. The frame buffer is **dedicated VRAM in separate banks**, owned by DAFB, and the
Quadra 700 and 900 are *"the first Macintosh models to combine an MMU with built-in video using
frame buffers in separate banks of VRAM"* [1] p. 40. What gives the subsystem its speed is the
attachment: *"Because the frame buffer in the Macintosh Quadra 700 computer is connected directly
to the system bus, it works with the MC68040 to provide high drawing speed for all applications,
even those that don't use QuickDraw"* [1] p. 16. Apple positions the result as *"graphics
performance approaching that of the Macintosh Display Card 8•24GC"* [1] p. 16 — DAFB is **not** a
blitter or an accelerator: no command set, no BitBlt engine, is published or implied by any source
in evidence (§3.7).

Everything the part does lives behind one CPU-visible register window in NuBus slot $9's address
space (§2.1). Behind that window sit four cooperating blocks:

- **the DAFB core** — frame-buffer base and row stride, mode configuration, the monitor-sense lines,
  a block-write control register, the SCSI timing-assist ("TurboSCSI") controls and a version/test
  register (§2.2);
- **the CRTC/timing generator** — a programmable horizontal and vertical counter set with three
  scanline interrupts, conventionally called **Swatch** in modern reverse engineering; the name is
  not Apple-published (§2.3);
- **the RAMDAC** — the Apple **AC842** palette/DAC on the Quadra 700 and 900, revised as the
  **AC842a** on the Quadra 950 with its 16-bit "Thousands" mode (§2.4);
- **the pixel-clock synthesizer** — a National Semiconductor **DP8531** programmable clock
  generator (§2.5).

DAFB also generates *"the timing and control signals to interface the [NCR 53C96] to the system
bus"* [1] p. 11 — the SCSI controllers of all three machines sit behind DAFB's timing-assist logic,
and their access-timing controls live inside DAFB's register file (§3.6).

### 1.2 Machines that carry it

| Machine | Project/board name | System bus | VRAM shipped | VRAM max | RAMDAC | Max depth |
|---|---|---:|---:|---:|---|---|
| Macintosh Quadra 700 (1991) | Spike [6] | 25 MHz | 512 KB (1 bank) | 2 MB (4 banks) | AC842 | 24-bit |
| Macintosh Quadra 900 (1991) | Eclipse [5] | 25 MHz | 1 MB (2 banks) | 2 MB (4 banks) | AC842 | 24-bit |
| Macintosh Quadra 950 (1992) | Eclipse board, revised video [3] p. 6 | 33.333 MHz | 1 MB | 2 MB | AC842a | 16-bit on large monitors, 24-bit below |
| Workgroup Server 95 (1993) | Quadra 950 board | 33.333 MHz | as Q950 | as Q950 | as Q950 | as Q950 |

The 700/900 VRAM organization is Apple's: the 700's frame buffer *"comprises four banks of video
RAM (VRAM); the basic configuration has 512 KB of VRAM installed in the first of the four banks"*,
each of the other three banks taking a pair of 256 KB VRAM SIMMs (the same SIMMs as the Macintosh
LC and the Display Card 4•8) [1] p. 15; the 900's basic configuration fills two of the four banks
[2] p. 13, and Apple's 700-vs-900 comparison table records the asymmetry — 700: one bank soldered,
three for expansion; 900: two banks soldered, two for expansion [1] Table 1-4 p. 26. The 950 keeps
the tower board but moves to 80 ns VRAM (against the 900's 100 ns) and runs a faster system bus
[3] p. 6; its depth ceilings differ because its RAMDAC does (§2.4.4, and the full depth/VRAM
tables in [dafb-modes.md](dafb-modes.md)).

The Quadra 950 is, in Apple's framing, *"basically a higher-performance Macintosh Quadra 900"*:
the video controller *"has been reprogrammed to take advantage of the faster VRAM, speeding up
video performance by as much as 20%*, on top of the 33 MHz processor's contribution — *"the
cumulative increase in video performance can be as much as 50% overall"* [3] p. 6. Note what that
sentence implies and what it does not say: the 950's part is a timing-revised configuration of the
same functional design, not a new architecture; whether it is a new mask part or a reprogrammed
stepping of the same silicon is not established by the note (§6).

### 1.3 Family context

These three machines are one motherboard architecture — MC68040 on the system bus, the Apple **MCU**
memory controller, the **JDB/Relayer** I/O-bus bridge, the **YANCC** NuBus controller, DAFB, and
SONIC Ethernet — described per machine in [1] and [2]. See the family page
([mcu.md](mcu.md)) for the shared board architecture, and the per-machine pages
([q700.md](q700.md), [q900.md](q900.md), [q950.md](q950.md)) for each machine's wiring. DAFB's
video lineage — on-board successor to the JMFB/Elmer cards, the first Apple on-board part to drive
the multiple-scan displays — is summarized in
[video-overview.md](../../hardware/video-overview.md) §3.2.

### 1.4 Clocking

DAFB sits on the system bus (25 MHz on the 700/900, 33.333 MHz on the 950 [3] p. 5), and its
TurboSCSI timing bits are counted in system clocks (§3.6). The scan side is clocked independently:
the DP8531 synthesizer produces the pixel clock under register control (§2.5), the AC842 divides
it (§2.4.3), and the CRTC's counters run from the result (§3.3) — which is how one fixed register
file drives everything from 512×384 to 1152×870, portrait, and interlaced composite timings.

## 2. Register file

### 2.1 The slot-$9 apertures

*"The control registers in the DAFB IC and the frame-buffer VRAM are mapped into the memory
locations that were assigned to NuBus slot $9 in earlier Macintosh models"* [1] p. 16; the 900
note repeats the assignment [2] p. 14. Built-in video is therefore a **pseudo-slot device**: the
Slot Manager treats it as a resident of slot 9, polling it with the expansion slots at interrupt
time (§3.4). The conventional standard-slot decode is `$F9000000 | (9 << 24)`:

| Range | Size | Function |
|---|---:|---|
| $F9000000–$F91FFFFF | 2 MB | VRAM aperture (CPU-visible frame buffer) |
| $F9800000–$F98003FF | 1 KB | DAFB register window |

The 2 MB VRAM window matches the largest installable configuration; the register window's four
256-byte sub-blocks are:

| Offset | Block |
|---|---|
| +$000–+$0FF | DAFB core: frame buffer, configuration, monitor sense, TurboSCSI, version/test |
| +$100–+$1FF | Swatch CRTC: timing and interrupts |
| +$200–+$2FF | AC842/AC842a RAMDAC: CLUT and pixel format |
| +$300–+$3FF | DP8531 clock generator |

Register-level facts in §2.2–§2.5 come from the modern register-level reconstruction of the part,
cross-checked against the boot ROM's own programming, which touches every block (§4.2); Apple
published the block functions [1][2] but no DAFB register specification (§6). Where a value's
behaviour has been *observed* in the ROM's access trace it is marked; everything else from this
reconstruction is marked *inferred — unverified* or listed in §6. The implemented value fields are
12 bits wide in the reconstruction, and the ROM's writes never exceed 12 bits (*observed*, [6]).

The architectural superslot space for slot 9 ($90000000–$9FFFFFFF) exists in the address map
[1] Figure 1-3 p. 14; whether physical DAFB decodes any useful alias there is not established
(§6).

### 2.2 DAFB core registers (+$000)

All offsets from $F9800000.

| Offset | Name (reconstruction) | Access | Function |
|---:|---|---|---|
| $000 | FB_BASE_HI | R/W | frame-buffer base, bits 20–9 |
| $004 | FB_BASE_LO | R/W | frame-buffer base, bits 8–5; base is 32-byte aligned |
| $008 | ROW_STRIDE | R/W | row stride in 32-bit words |
| $00C | TIMING_CTRL | R/W | stored; bit meanings not established |
| $010 | CONFIG | R/W | bit 2 interlace, bit 3 convolution; other bits not established |
| $014 | BLOCK_WRITE | R/W | block-write control latch; semantics unresolved (§3.7) |
| $018 | — | R/W? | unknown |
| $01C | MONITOR_SENSE | R/W | sense-line drive (write) and sampled lines (read) |
| $020 | — | R/W | unknown; written by the boot ROM |
| $024 | TURBOSCSI0 | R/W | SCSI controller 0 access timing / DRQ status (§3.6) |
| $028 | TURBOSCSI1 | R/W | SCSI controller 1, on the dual-SCSI machines (§3.6) |
| $02C | TEST_VERSION | R | test state (low bits) + DAFB revision (upper bits) |

**Frame-buffer base ($000/$004).** The base is a 21-bit offset **into the VRAM aperture**, not an
absolute system address, assembled from the two registers:

```c
fb_base = ((base_hi & 0x0FFF) << 9) | ((base_lo & 0x000F) << 5);
fb_base &= 0x001FFFE0;   // within the 2 MB aperture, 32-byte aligned
```

The 32-byte granularity is implied by the register split (LO carries nothing below bit 5). The
Quadra 700 boot ROM's first mode set programs $000 = 8, $004 = 0 — base offset $1000 (*observed*,
[6]); a second set later in the same boot programs $000 = $0A, $004 = 1 — base $1420 (*observed*,
[6]). A non-zero base is how the screen origin is panned inside the installed VRAM.

**Row stride ($008).** Stored in 32-bit words: `row_bytes = value << 2` (*inferred — unverified*;
consistent with every observed program, in which stride values are word counts). The stride may
exceed the visible row width — the boot ROM's first set programs stride $100 (256 words = 1024
bytes) for a 640-wide mode (*observed*, [6]) — so a scanout line's fetch is `row_bytes` wide
regardless of the visible width. What the scanout does when `base + row_bytes × height` runs past
installed VRAM is not established (§6).

**Timing control ($00C).** The ROM writes $61E in its first mode set and $60E in its second
(*observed*, [6]) and clears the register to 0 while switching (*observed*, [6]). No bit of it is
decoded by any source in evidence. Its placement between the stride and the configuration
registers suggests VRAM/scan timing, but that is naming-convention inference and it is **not**
asserted as fact.

**Configuration ($010).** The reconstruction assigns bit 2 = interlace and bit 3 = Apple
convolution. Both assignments are corroborated by the trace: the ROM's second mode set writes $7C
then $87C — bits 2 and 3 set together with bits 4–7 and 11 — and that same set programs an
**odd** vertical half-line total and a PCBR0 divide of 2, the signature of an interlaced,
convoluted composite timing (§3.3, §4.2) (*observed*, [6]; the bit mapping itself is
*inferred — unverified*). Bit 11 ($800) is set in both observed mode sets and is the register's
other recorded constant (*observed*, [6]); its meaning is unknown. The ROM reads $010 before
programming anything (*observed*, [6]) — an apparently deliberate power-state probe — and writes
$800 alone before the full value in both sets (*observed*, [6]).

**Block write ($014).** See §3.7. The register is a latch; nothing more is established.

**Monitor sense ($01C).** See §3.5.

**TurboSCSI ($024/$028).** See §3.6.

**Version/test ($02C).** A 12-bit register whose low bits carry test state and whose upper bits
carry the DAFB revision: the reconstruction places the version field at bits 11:9 and assigns
values 0, 1 or 2 to the Quadra 700/900's part and 3 to the Quadra 950's (*inferred — unverified*
— no Apple document names the field or its placement). The exact bit positions are not
hardware-verified; §6. The boot ROM reads $02C during video init (*observed*, [6]) — so the
register must answer from power-on, before any mode is programmed. Software distinguishes the
950's video hardware from the 700/900's by machine identity (Gestalt machine type 26 vs. 20 [3]
p. 8), not solely by this register; the version register's actual consumer is not identified in
the evidence.

### 2.3 Swatch CRTC registers (+$100)

The timing generator occupies the second 256-byte sub-block. Its registers are 12-bit. The
mnemonics below are the reconstruction's (Apple published none of them); the horizontal names
follow broadcast-signal practice (equalization, serration, breezeway, burst), consistent with a
part that must generate NTSC/PAL-compatible sync.

| Offset | Name | Access | Function |
|---:|---|---|---|
| $100 | MODE | R/W | bit 0 modelled as display disable; other bits unknown |
| $104 | IRQ_ENABLE | R/W | bit 0 VBL; bit 1 auxiliary scanline; bit 2 cursor scanline |
| $108 | IRQ_STATUS | R | bit 0 VBL pending; bit 2 cursor pending |
| $10C | CLEAR_CURSOR | side effect | clears the cursor-pending bit |
| $110 | — | R/W? | unknown |
| $114 | CLEAR_VBL | side effect | clears the VBL-pending bit |
| $118 | CURSOR_LINE | R/W | scanline targeted by the cursor interrupt |
| $11C | AUX_LINE | R/W | scanline targeted by the auxiliary interrupt |
| $120 | SCRATCH | R/W | scratch/test latch |
| $124 | HSERR | R/W | horizontal serration-pulse point |
| $128 | HLFLN | R/W | half-line point (equalization/serration) |
| $12C | HEQ | R/W | horizontal equalizing-pulse point |
| $130 | HSP | R/W | horizontal sync-pulse point |
| $134 | HBWAY | R/W | horizontal breezeway point |
| $138 | HBRST | R/W | horizontal burst-window point |
| $13C | HBP | R/W | horizontal back-porch point |
| $140 | HAL | R/W | horizontal active-display start |
| $144 | HFP | R/W | horizontal active-display end (start of front porch) |
| $148 | HPIX | R/W | total horizontal count, minus 2 |
| $14C | VHLINE | R/W | total vertical half-lines per field; odd value = interlace convention |
| $150 | VSYNC | R/W | vertical sync point |
| $154 | VBPEQ | R/W | vertical back-porch equalization point |
| $158 | VBP | R/W | vertical back-porch start |
| $15C | VAL | R/W | vertical active-display start |
| $160 | VFP | R/W | vertical active-display end (start of front region) |
| $164 | VFPEQ | R/W | total vertical half-lines (end equalization point) |

The geometry the reconstruction derives from these registers:

```text
h_total        = HPIX + 2
visible_width  = HFP - HAL
visible_height = (VFP - VAL) / 2
v_total        = VFPEQ / 2
```

These derivations are *inferred — unverified* as silicon specification, but they are validated
against the boot ROM's own programming: the ROM's first mode set programs HAL = $88, HFP = $308
(visible width 640), VAL = $44, VFP = $404 (visible height 480), VFPEQ = $408 (525-class vertical
total) — a 640×480 timing — and HPIX = $31E (h_total 800) (*observed*, [6]). The vertical counts
are in **half-lines**: the active height is the half-line span divided by two, and the odd/even
parity of VHLINE carries the interlace convention (the second observed set programs VHLINE = $271,
odd, with the interlace and convolution configuration bits set — an interlaced composite class of
timing) (*observed*, [6]).

How the horizontal counts relate to output dots at the deeper pixel-clock divides (whether a
count unit is one dot or several) is not established by the evidence; the reconstruction treats
the AC842's divide (§2.4.3) as scaling the dot rate rather than the counter units, and the
evidence cannot distinguish the two models (§6).

The interrupt registers are treated in §3.4. The two clear registers ($10C, $114) clear their
pending bits on **access**; whether a read, a write, or both clear is not settled (§6) — the boot
ROM only ever writes them (*observed*, [6]).

### 2.4 AC842 / AC842a RAMDAC registers (+$200)

The palette and pixel-format block occupies the third sub-block. Three registers are mapped:

| Offset | Name | Function |
|---:|---|---|
| $200 | CLUT_ADDR | 8-bit CLUT entry index |
| $210 | CLUT_DATA | palette data; sequential R, G, B components, auto-increment after blue |
| $220 | PCBR0 | pixel depth and pixel-clock divide (reconstruction name) |

plus, on the Quadra 950's AC842a only, an **indirectly selected extended control register**
(PCBR1), reached through the address register rather than a fourth mapped offset (§2.4.4).

#### 2.4.1 Palette protocol

A full palette write is: index to $200, then three component bytes to $210 — red, green, blue in
order — after which the entry index auto-increments (*inferred — unverified*; exactly matches the
boot ROM's palette fill, which writes index 0 to $200 then six bytes to $210: FF FF FF, then
00 00 00 — entries 0 = white and 1 = black, the 1-bpp gray-screen pair, on consecutive triplet
boundaries) (*observed*, [6]). Reads are modelled as walking the same component state machine;
readback support on real silicon is not established (§6).

The reconstruction also carries a monochrome convention: on monochrome display profiles the blue
component is used as the intensity and replicated to all three DAC outputs, matching Macintosh
monochrome DAC wiring (*inferred — unverified*).

#### 2.4.2 Pixel depth (PCBR0 bits 3:2 of the low byte — mask $1C)

| PCBR0 & $1C | Storage/display mode |
|---:|---|
| $00 | 1 bit per pixel, indexed |
| $08 | 2 bits per pixel, indexed |
| $10 | 4 bits per pixel, indexed |
| $18 | 8 bits per pixel, indexed |
| $1C | direct colour — 24-bit RGB in 32-bit storage (or 16-bit x555 on an AC842a with the extension enabled, §2.4.4) |

The five values above are the only ones the reconstruction defines; intermediate patterns (e.g.
the $06 the boot ROM writes during its RAMDAC probe, §4.2) are deliberately not coerced to a
nearest mode (§5). The indexed modes pack pixels most-significant first within each byte and word
(§3.2).

#### 2.4.3 Pixel-clock divide (PCBR0 bits 6:5 — mask $60)

```c
pixel_divide = 1u << ((pcbr0 & 0x60) >> 5);   // 1, 2, 4 or 8
```

The scanout dot clock is the DP8531 output divided by this value (*inferred — unverified*). The
boot ROM's first mode set leaves the divide at 1 (PCBR0 = $80 — depth 1 bpp, divide 1, bit 7 set,
meaning unknown) and its second selects 2 (PCBR0 = $21 — 1 bpp, divide 2, bit 0 set, meaning
unknown) (*observed*, [6]).

#### 2.4.4 The AC842a extension (Quadra 950)

The Quadra 950 *"adds support for 16 bits per pixel"* — *"The 16 bit-per-pixel video displays
provide up to 32,768 colors"*, selected by the user in the Monitors control panel [3] p. 6. At
register level the reconstruction attributes this to the AC842a's hidden control register
(PCBR1), selected through a special address/control sequence rather than a fixed offset
(*inferred — unverified*). When the extension is active and PCBR0 selects direct colour, pixels
are big-endian 16-bit words:

```text
bit 15 ignored | bits 14–10 red | bits 9–5 green | bits 4–0 blue
```

with 5-bit components expanded to 8-bit DAC output by bit replication (*inferred — unverified*).
The exact indirect-selection sequence is not established from Apple's published note; §6. The
950's depth/VRAM ceiling tables are in [dafb-modes.md](dafb-modes.md).

### 2.5 DP8531 clock generator registers (+$300)

The pixel clock is synthesized by a National Semiconductor DP8531 programmable clock generator
[8], reached through the fourth sub-block. The reconstruction maps it as **sixteen 4-bit
registers**:

- register number = `(offset >> 4) & 0xF`, i.e. register *n* at $300 + n×$10;
- only one byte lane of each longword slot carries data (the modelled lane is the one with
  `(offset & 3) == 3`) (*inferred — unverified*);
- writing **register 15 commits**: the synthesizer output is recalculated from the standing
  nibble set at that point, not at each nibble;
- readback is not established (§6).

The frequency arithmetic of the reconstruction:

```c
R       = (reg[6] << 8) | (reg[5] << 4) | reg[4];        // reference divider
P       = 1u << reg[9];                                  // output divider
modulus = (reg[3] << 12) | (reg[2] << 8) | (reg[1] << 4) | reg[0];
A       = (~modulus) & 0x1F;
B       = max((modulus >> 5) & 0x7FF, 2);
if (A > B) A = B;
N       = 32 * (B - A) + 31 * (1 + A);
vco     = (20,000,000.0 / R) * N;
output  = vco / P;
```

The formula is *inferred — unverified* as a register specification, but it is consistent with the
part's documented synthesizer structure [8] and it reproduces a sensible pixel clock from the
ROM's own programming: the first mode set's nibble program (reg0 = $F, reg1 = 1, reg2 = 1,
reg3 = 0, reg4 = 9, reg5 = 3, reg6 = 0, reg9 = 2, reg10 = 5, reg11 = 6, reg12 = 4, reg13 = 1,
reg15 = 0) (*observed*, [6]) yields ≈ 24.5 MHz — which clocks the same set's 800×516 timing at
≈ 59 Hz, a correct 640×480-class refresh. The second set's program (reg0 = $D, reg1 = 9,
reg2 = 1, reg3 = 0, reg4 = 6, reg5 = 4, reg6 = 0, reg7 = 1, reg8 = 1, reg9 = 1, reg10 = 5,
reg11 = 6, reg12 = 0, reg13 = 1, reg15 = 0) (*observed*, [6]) commits a roughly 57.6 MHz
synthesizer output, divided by 2 by the AC842 for the composite-class mode (§4.2).

An illegal program (R = 0, or a partially written nibble set) must not be trusted to produce a
clock; the guard behaviour of real silicon is unknown (§6).

### 2.6 Reset state

No source in evidence documents DAFB's power-on register values. What can be said:

- The **version/test register must answer reads from power-on** — the boot ROM reads $02C before
  programming any mode (*observed*, [6]).
- The **sense lines must read as undriven at power-on** — the ROM's very first access to $01C is
  a *read*, taken before it writes the register, and the passive monitor code must be the answer
  (*observed*, [6]; the register's drive bits wake tri-stated — *inferred* from that read's
  purpose). A register whose drive bits woke asserted would return a wrong passive code and the
  ROM would size the wrong monitor class (§5).
- The **configuration register reads 0 before the first write** (*observed*, [6]) — on the
  captured run this is the logging stub's echo as much as silicon fact; it is consistent with a
  cleared power-on state but does not prove one.
- The rest of the file's reset values — Swatch timing, stride, base, PCBR0, DP8531 nibbles — are
  not established (§6).

## 3. Behaviour

### 3.1 Address decode and the VRAM aperture

DAFB decodes the two slot-9 apertures of §2.1 and nothing else that is established: VRAM at
$F9000000, registers at $F9800000. The VRAM window is 2 MB regardless of installed capacity;
configurations install 512 KB, 1 MB or 2 MB. Above the installed capacity the window's behaviour
is not established — wrap/alias, open bus, or zero are all consistent with the evidence
(*unverified*; §6). The frame buffer is big-endian from the 68040's viewpoint.

Because the frame buffer lives in slot space rather than main RAM, the ROM's memory manager
*"manages the address space for the video frame buffers separately from main memory"* [1] p. 40 —
there is no interaction between VRAM sizing and RAM sizing, and no shared-memory arbitration
between DAFB scanout and the processor as there is on RAM-based-video machines. Contention inside
DAFB — CPU frame-buffer accesses against scanout refresh inside the VRAM controller — is not
documented at register level (§6).

### 3.2 Pixel formats

**Indexed modes (1, 2, 4, 8 bpp).** Pixels are packed most-significant first within each byte
and each word: in 1 bpp, bit 7 of the first byte is the leftmost pixel; in 4 bpp, the high nibble
is the left pixel. The scanout fetches `row_bytes` per line at the programmed stride (§2.2) and
presents the literal CLUT index — QuickDraw colour conventions (grey ramps, index scaling) are
software's business, applied through the palette, not by the DAC.

**Direct colour (24-bit).** Each pixel occupies a full 32-bit word of storage — the "24-bit"
modes are 32-bpp frame buffers as far as VRAM consumption is concerned (see
[dafb-modes.md](dafb-modes.md) "The 24-bit nuance"). The reconstruction treats the pixel as RGB
in the low three bytes of the big-endian word; whether the unused byte leads or trails — and
whether it is ignored or carries format state — is not established (*inferred — unverified*; §6).

**Thousands (16-bit, Quadra 950 only).** Big-endian x555 words per §2.4.4 [3] p. 6.

**Convolution.** With the convolution configuration bit set, DAFB produces television-compatible
output — Apple documents convolution as the flicker-reduction facility for NTSC/PAL modes on
these machines [1] p. 15 — and *"changes how source pixels map to displayed pixels"*. The filter's
actual resampling behaviour is not specified by any source in evidence (*unverified*; §6).

### 3.3 Scan timing generation

The Swatch block is a two-dimensional counter set: horizontal counters clocked from the pixel
clock (DP8531 output ÷ the AC842 divide), vertical counters in half-lines. A frame is: h_total
counts wide (HPIX + 2), v_total half-line counts tall (VFPEQ / 2, with VHLINE's parity carrying
the interlace convention), active video between HAL→HFP horizontally and VAL→VFP vertically,
sync/equalization/serration points at the remaining registers. The broadcast-named registers
(HSERR, HEQ, HBWAY, HBRST) exist because the part must generate NTSC/PAL-class composite sync, not
just RGB monitor timings.

The exact unit conventions — inclusive vs. exclusive endpoints, off-by-one placement of the
sync points, how interlace splits a frame into two fields — are reconstruction-derived and have
not been validated against hardware measurement (*inferred — unverified*; §6). The register
*values* the ROM programs for its modes are directly observed and are listed in §4.2.

Mid-frame reprogramming behaviour (whether a changed HPIX takes effect mid-scanline, at line
boundary, or at frame boundary) is not established. The ROM avoids the question by blanking first
(§4.2) — a driver contract worth copying.

### 3.4 Interrupts

DAFB generates three scanline-positioned interrupt sources, all in the Swatch block (§2.3):

- **vertical blanking (VBL)** — raised at the start of the vertical blanking interval, derived
  from the programmed vertical timing;
- **cursor line** — raised when the scan reaches the scanline programmed in $118;
- **auxiliary line** — raised at the scanline programmed in $11C. The reconstruction names it an
  "animation" line; its intended use is not documented by any source in evidence (§6).

Each has an enable bit ($104) and a pending bit ($108); the two clear registers ($10C, $114)
acknowledge the cursor and VBL sources respectively. The reconstruction's rule — pending bits
set from the timing regardless of the enable mask, and the output driven by
`status & enable` — is *inferred — unverified* in its details (whether status latches when
disabled, and whether clears act on read, write, or either access, are all open; §6).

**Output routing.** DAFB's interrupt output is the built-in-video slot interrupt: it reaches
**VIA2 port A bit 6, "Video IRQ"** — Table 2-6 in both notes [1] p. 37, [2] p. 40 — and joins the
slot interrupt lines through the OR gate that produces **/SLOTIRQ**, wired to VIA2 CA1; VIA2 then
raises a **level 2** interrupt on the MC68040 [1] pp. 36–37. The dispatcher *"polls the second
VIA, bits PA0 through PA6, to determine which slot generated the interrupt"* [1] p. 37 — built-in
video is simply slot 9 in that poll. Apple's design guidance for that poll is explicit: the
slot /IRQ should be **latched** so that *"once it is asserted, the interrupt handler software for
the card has the responsibility of clearing the interrupt"*, ensuring the line is still asserted
when polled [1] p. 37. DAFB satisfies this by holding its output as a **level** until the Swatch
status is acknowledged (*inferred — unverified*; consistent with the routing and Apple's latching
guidance, and the reconstruction models it level-sensitive). The boot ROM's cursor-line exercise
(§4.2) — enable, wait for pending, clear — is the minimal contract: pending bits must latch, and
clearing must drop the output.

### 3.5 Monitor sense

The video connector carries **three sense lines**. DAFB drives and samples them through $01C:
a write's low three bits select which lines the host pulls low and which are released; a read
returns the resulting line states, inverted as the register interface presents them
(*inferred — unverified* for the bit polarity; the probe *sequence* is directly *observed*, below).
A monitor signals its type by grounding the lines of its code through its cable.

The passive (three-line) codes:

| Sense code | Display |
|---:|---|
| 000 | 21-inch colour two-page, 1152×870 |
| 001 | portrait/full-page monochrome, 640×870 |
| 010 | 12-inch colour, 512×384 |
| 011 | 21-inch monochrome |
| 100 | NTSC |
| 101 | legacy 15-inch colour / full-page |
| 110 | 13-inch high-resolution colour, 640×480 — **or** an extended-protocol display (type 6) |
| 111 | no display — **or** an extended-protocol display (type 7) |

The code assignment is the standard Macintosh sense-line scheme documented for display
designers [7]; the reconstruction's table matches it. Codes 110 and 111 are ambiguous on three
lines alone, which is why the protocol has a second stage: **extended sense**. For an
extended-sense monitor, the host **drives one line low and reads the other two**, cycling through
the three lines, and the monitor's internal tie matrix answers with a 6-bit code [7].

The boot ROM performs exactly this dialogue, and the whole shape of it is *observed* in the
access trace [6]:

1. read $01C with all lines released — the passive code;
2. write $07 (all lines released), read;
3. write $03 (line 2 driven), read; write $07, read;
4. write $05 (line 1 driven), read; write $07, read;
5. write $06 (line 0 driven), read; write $07, read.

That is: one passive read, then a drive-one/read-two sweep over all three lines with the lines
released between steps. A display cannot be modelled as a static three-bit ID — the ROM probes
dynamically, and a monitor whose extended answers are wrong gets programmed as the wrong model
entirely (the captured trace's stub monitor, answering zero to everything, steered the ROM into
an interlaced composite-class mode set; §4.2). The sense lines' electrical detail — drive
strength, exact tri-state timing, behaviour on contradictory answers — is not established (§6).

### 3.6 TurboSCSI — the SCSI timing assist

All three machines hang their NCR 53C96 SCSI controllers off DAFB, which *"generates the timing
and control signals to interface the 53C96 to the system bus"* [1] p. 11 — the Quadra 700 with one
controller (internal and external connectors on the same SCSI bus), the 900 and 950 with two
(separate internal and external buses, internal cabling good for 5 MB/s) [1] p. 11, [2] p. 13.
The register face of this is the two control registers $024 (controller 0) and $028
(controller 1), one per controller.

What TurboSCSI is **not** is as important as what it is: it is not bus-master DMA. The CPU moves
every SCSI byte itself through a pseudo-DMA data window; DAFB's contribution is (a) programmable
access timing — how many system clocks each NCR register or data access costs — and (b)
**DRQ-gating**: the ability to hold off transfer acknowledge on a data access until the NCR
controller asserts data request (*inferred — unverified* in its exact cycle behaviour; the bit
assignments below are reconstruction).

| Bit | Modelled meaning |
|---:|---|
| 0 | NCR register read: 6 system clocks (priority over bit 1) |
| 1 | NCR register read: 4 system clocks; bits 0–1 clear → 3 clocks |
| 2 | NCR register write: 3 clocks set, 4 clocks clear |
| 3 | pseudo-DMA read: 3 clocks set, 4 clocks clear |
| 4 | pseudo-DMA write: 5 clocks (priority over bit 5) |
| 5 | pseudo-DMA write: 3 clocks when bit 4 clear; both clear → unresolved (§6) |
| 6 | unknown (reconstruction comment "CS PW check") |
| 7 | DRQ-check on pseudo-DMA reads: hold the access until DRQ |
| 8 | DRQ-check on pseudo-DMA writes: hold the access until DRQ |
| 9 | live NCR DRQ status, read-only in the readback |

The DRQ-check bits are the performance story: with the check enabled, a pseudo-DMA access simply
waits — the CPU bus cycle is stalled, transfer acknowledge withheld — until the SCSI controller has
a byte ready, rather than the driver polling the DRQ bit in a register between every byte. If DRQ
never asserts, a timeout ends the cycle with a bus error; the timeout duration is not established
(§6). The boot ROM writes $41 to $024 during video init — bits 0 and 6 (*observed*, [6]) — so the
access-timing configuration is folded into the same init pass as the video mode set, which is a
reminder that this register pair lives in a *video* controller's file.

### 3.7 Block write

Apple's notes sell the subsystem's speed on the direct system-bus attachment and say nothing
about a fill engine [1] p. 16 — but QuickDraw on these machines *"sets the DAFB to its fastest
mode of operation (per display and per bit depth)"* [1] p. 47, which presupposes a programmable
operating mode of the frame buffer, and the register file has a control register for it: $014,
the block-write latch. Candidate behaviours consistent with the name and the era are VRAM
block-write cycles — one CPU access filling multiple VRAM locations, or colour-latch fill
semantics — but **no source in evidence establishes the trigger, the source/colour fields, the
span/stride rules, the completion condition or the supported depths**, and the reconstruction
deliberately does no more than store the written value (*unverified*; §6). What is established is
the negative: DAFB has no published blitter command set, and nothing in the evidence invites
inventing one.

## 4. Programming model

### 4.1 The ROM's video software

The ROM ships *"a new video driver to support the new hardware"* [1] p. 43. Two neighbouring
facts shape everything the driver does:

- The frame buffer's address space is managed separately from main memory [1] p. 40 — the driver
  sizes and owns VRAM through DAFB's registers, never through the memory manager's RAM map.
- The MC68040's data-bus behaviour differs from the 68020/68030 (no byte smearing), so the ROM
  patches the ROM software of Apple's **older** display cards — the Macintosh II Video Card and
  early Display Cards 4•8 and 8•24 — to work in these machines [1] p. 43. DAFB's own frame
  buffer needs no such patch; the driver for the built-in hardware is native.

The mode/depth catalogue exposed to the Slot Manager is carried as slot resources in the ROM's
declaration data (built-in video enumerates as slot 9), and QuickDraw's contribution is mode
selection: it simply sets the fastest operating mode per display and per depth [1] p. 47. The
user-facing depth choice on the 950 — picking Thousands in the Monitors control panel [3] p. 6 —
flows through the same driver into PCBR0 and the AC842a extension. The complete
resolution/VRAM/depth ceiling tables are on the mode page, [dafb-modes.md](dafb-modes.md); this
section is the register mechanics.

### 4.2 The boot-time initialization sequence (observed)

The following is the actual power-on sequence of the Quadra 700/900 ROM against DAFB's registers,
as captured in an instruction-level access trace of a Quadra 700 boot (8 MB RAM, no media, first
~60M instructions) [6]. The trace answers are from a logging environment, not a physical monitor —
the *structure* of the sequence is the evidence; the particular second mode it settles into is an
artifact of that environment's sense answers (§3.5).

1. **SCSI timing first.** Write $024 = $41 — TurboSCSI channel 0, bits 0 and 6 — before anything
   video happens.
2. **RAMDAC probe.** $200 = 0; $220 = $06; $200 = 1; $220 = 0; $200 = 0 — address 0, a $06 pixel
   control value, address 1, a 0, address 0 again. The reconstruction reads this as an
   AC842/AC842a presence probe: on a RAMDAC whose extended control register is selected through
   the address register, the $220-while-address-is-1 write is routed away from PCBR0, and reading
   PCBR0 back distinguishes the two parts (*inferred — unverified*; an alternative reading treats
   the same bytes as a block-write probe — §6). $06 is deliberately an *undefined* PCBR0 depth
   pattern, which is safe precisely because the probe reads back rather than displays.
3. **Read the configuration register** ($010 → 0), then write $800 (bit 11 alone).
4. **Program the pixel clock.** All sixteen DP8531 nibbles, register 0 through 15, the commit
   register last (§2.5's first program).
5. **Program the core.** $000/$004 base ($1000), $008 stride ($100 words = 1024 bytes), $00C
   timing ($61E), $010 = $30 then $830 — the full mode-1 configuration.
6. **Enable and program the CRTC.** $100 = $FF2 (mode register, display enabled), then the full
   timing set of §2.3's table — the 640×480-class values.
7. **Set the pixel format.** $200 = 0, $220 = $80 — 1 bpp, divide 1.
8. **Test/version read.** $02C → 0.
9. **The $020 sequence.** $020 = 3, 7, 3, 2, 0 — five writes to the unknown register bracketing
   the mode set. The register's function is unknown; the sequence is stable enough to be a
   handshake or test-state walk (*observed*; function *unverified*).
10. **Blank.** $00C = 0, $010 = 0, $100 = 1 — timing cleared, configuration cleared, display
    disabled: the machine is now between modes.
11. **Re-run the RAMDAC probe** (step 2's bytes, again).
12. **Mask video interrupts.** $104 = 0.
13. **Probe the monitor.** Read $01C (passive), then the extended-sense sweep of §3.5 —
    $07, $03, $07, $05, $07, $06, $07 with reads interleaved.
14. **Program the second mode** — the one the sense answers selected: $010 = $800; the second
    DP8531 program (commit last); base $1420; stride $200 words (2048 bytes); $00C = $60E;
    $010 = $7C then $87C (interlace + convolution bits set); $100 = $FF2; the second Swatch
    timing set (interlaced composite class, VHLINE odd); $200 = 0, $220 = $21 (1 bpp,
    divide 2); and the $020 sequence again.
15. **Load the palette.** $200 = 0, then $210 = FF, FF, FF, 00, 00, 00 — entry 0 white, entry 1
    black, two triplets on the auto-increment.
16. **Exercise the cursor interrupt.** $118 = $134 (line 308); $104 = 4 (cursor enable only);
    $10C = 0 (clear); read $108 → 1 — a pending bit latched and read back. The palette fill is
    then repeated twice more, with a single write $018 = 1 in between (function unknown).

Two complete mode sets in one boot, each in the same order — SCSI timing, RAMDAC probe, clock,
core, CRTC, pixel format — with the monitor probed only between them. The first set is the
ROM's bring-up raster (640×480-class, monochrome), programmed before the monitor is known so
there is a display as early as possible; the second is the monitor's own mode. The trace window
ends inside early boot; everything the Slot Manager, Monitors panel and QuickDraw do later
(mode changes, depth switching, palette animation) follows the same register contracts with the
same ordering discipline: blank, reprogram, enable.

### 4.3 Mode changes

A mode or depth change is a re-run of §4.2's core sequence: disable display ($100 bit 0), clear
the timing/configuration, reprogram the DP8531 (commit last) and the Swatch set, set base/stride
and the pixel format, re-enable. The blanking step is the ROM's own practice (*observed*, [6])
and is the only ordering rule the evidence supports; whether silicon tolerates live reprogramming
is not established (§3.3).

### 4.4 Depth and palette

Depth is PCBR0's $1C field (§2.4.2), with the AC842a extension for the 950's Thousands mode
(§2.4.4). Palette updates are index-then-triplet at any time (§2.4.1); when the entry index is
left to auto-increment, runs of entries load as consecutive triplets, which is how the ROM loads
its gray-screen pair in six data writes (*observed*, [6]). Nothing in the evidence establishes
whether CLUT updates are visible mid-scan or latched to blanking (§6).

## 5. Quirks & errata

- **A video controller with SCSI inside.** The NCR 53C96 timing/DRQ controls live in DAFB's
  register file ($024/$028) and are programmed by the same init pass [1] p. 11, *observed* [6].
  Software that treats DAFB as video-only misses half its register file.
- **The "DAFB II" label conflict.** The surviving Quadra 700 main-logic-board schematic titles
  its DAFB symbol "DAFB II" [4] — yet the 700's part is functionally the *pre-revision* one
  (reconstruction versions 0–2, no x555), and the name "II" belongs to the 950's feature set by
  behaviour. A package/schematic-revision label, an early naming convention, or a stepping name
  — unresolved (§6). Do not read colour capability off schematic text.
- **"24-bit" is 32 bits of storage.** Every direct-colour pixel costs a longword of VRAM
  (§3.2, [dafb-modes.md](dafb-modes.md)) — the depth tables are VRAM-consumption statements as
  much as colour statements.
- **Undefined PCBR0 depth patterns are used by the ROM itself.** The $06 probe value (§4.2
  step 2) is not one of the five defined depth encodings; a reader must not coerce such
  patterns to a nearest mode, or the probe's readback changes.
- **The RAMDAC probe has two live readings.** The $200/$220 byte sequence at the start of each
  mode set (§4.2 step 2) is read either as an AC842/AC842a presence probe (address-selected
  extended control register) or as a block-write facility probe. Both readings exist in modern
  reconstruction; the evidence does not settle between them (§6).
- **The sense register must not echo its own writes.** A read of $01C returns the *line states* —
  what the monitor holds — not the last drive value. A register that read back its own write
  would defeat both the passive read and the extended sweep, and the ROM would size the wrong
  display class (the trace's zero-echo stub steered it into an interlaced composite mode;
  §3.5, §4.2).
- **A DRQ-gated pseudo-DMA access can out-wait the CPU.** With TurboSCSI bits 7/8 set, an access
  to the SCSI data window is held until the controller signals DRQ, ending in a bus error only at
  timeout (§3.6) — software cannot assume the access completes promptly.
- **The vertical counters count half-lines.** Visible height is a half-line difference divided
  by two, and VHLINE's parity is the interlace convention (§2.3) — off-by-two errors in a
  derived model are the norm, not the exception, until validated against hardware.
- **The frame-buffer base is an offset, not an address.** The 21-bit base of $000/$004 indexes
  the 2 MB VRAM aperture (§2.2); treating it as a system address breaks the moment VRAM is not
  at the aperture start of a larger mapping.
- **One machine, one ROM, two video personalities.** The 700 and 900 share a boot ROM image, and
  the 950 has its own [9]; the 950's Thousands capability is selected by machine identity and
  the RAMDAC's presence probe, never by the version register alone (§2.2, §2.4.4).
- **The 512×384 active-height oddity.** Modern reconstruction reports the 12-inch 512×384
  programming as one active line taller than the mode it declares, inconsistent with the other
  modes' exact arithmetic (*observed in reconstruction, unresolved*) — see §6.

## 6. Open questions

1. **Timing control ($00C) bits.** The ROM's $61E/$60E values are stable per mode but no bit is
   decoded by any source. VRAM/scan timing is the natural guess and remains a guess.
2. **Block write ($014).** Trigger semantics, source/colour fields, span/stride rules, supported
   depths, completion, and whether any IRQ reports it. Also: whether the boot ROM's $200/$220
   probe sequence (§4.2) is *this* facility's probe or the RAMDAC's (§5).
3. **Swatch unit conventions.** Exact scaling and inclusive/exclusive endpoints of the
   horizontal and vertical counters; off-by-one placement of the sync/equalization points;
   whether horizontal counts are dots or divided "characters" at AC842 divides above 1 (§2.3);
   mid-frame update behaviour.
4. **Interrupt fine print.** Whether pending bits latch while their enable is clear; whether the
   clear registers act on read, write, or either; the aux-line interrupt's intended purpose; and
   the exact scanline→pending relationship for the cursor line.
5. **The version/test register ($02C).** Exact bit placement of the version field, the meaning of
   the low test bits, the answer each board stepping actually gives, and which software consumes
   it.
6. **Reset values** of the whole register file — Swatch, stride, base, PCBR0, DP8531 nibbles —
   beyond the three behaviors of §2.6.
7. **The "DAFB II" schematic label** on the Quadra 700 [4]: package marking, stepping name, or
   early naming convention — and whether any board revision's hidden behaviour differs.
8. **Out-of-range VRAM** behaviour above installed capacity (wrap, open bus, zero, or partial
   decode), per capacity; and the superslot-9 alias question — whether anything useful decodes
   at $90000000-class addresses.
9. **AC842/AC842a detail.** The exact indirect-selection sequence for PCBR1; CLUT readback
   support; the mono blue-intensity convention; power-on palette state; and the unused
   byte's role in 32-bit direct-colour storage (leading vs. trailing, ignored vs. format bits).
10. **DP8531 detail.** Real readback behaviour; the exact serial/latch protocol behind the
    nibble-register view; illegal-field handling; whether every board revision programs it
    identically.
11. **Monitor-sense electrical model.** Drive strength and tri-state timing, bit polarity per
    stepping, and behaviour on contradictory extended-sense answers.
12. **TurboSCSI loose ends.** Bits 4–5 both-clear write timing; bit 6 ("CS PW check"); the
    DRQ-check timeout duration and its error side effects.
13. **The $020 register** and the ROM's 3-7-3-2-0 write sequence, and the $018 register's single
    observed write of 1 — both completely undecoded.
14. **The 12-inch 512×384 one-line discrepancy** in the ROM's active-height arithmetic (§5).
15. **The 950's silicon identity**: new mask part or reprogrammed stepping of the same part —
    Apple's note says "reprogrammed" [3] p. 6 and nothing more; the Workgroup Server 95's video
    is assumed to be the Q950 board's but is independently verified by nothing in the evidence
    set.
16. **Scanout arbitration** inside the VRAM controller — CPU frame-buffer writes against refresh,
    and any stall behaviour visible to software — is entirely undocumented.

## References

1. Apple Computer, Inc., *Developer Note: Macintosh Quadra 700*, Developer Technical Publications,
   1991 — §"Design architecture" p. 4; §"Custom ICs" p. 6; §"SCSI controller IC" p. 11;
   §"Video frame-buffer controller IC: DAFB" pp. 15–16, Table 1-1 p. 16; Figure 1-3 NuBus address
   map p. 14; Table 1-4 (comparison with the Quadra 900) p. 26; §"Interrupt handling" pp. 36–37,
   Table 2-6 p. 37; §"Virtual memory" (MMU + separate frame-buffer address space) p. 40;
   §"ROM support for the video hardware" p. 43; §"Enhancements to QuickDraw" p. 47.
2. Apple Computer, Inc., *Developer Note: Macintosh Quadra 900*, Developer Technical Publications,
   1991 — §"NuBus controller IC: YANCC" and §"Video frame-buffer controller IC: DAFB" pp. 13–14;
   Figure 1-3 p. 14; §"Interrupt handling", Table 2-6 p. 40.
3. Apple Computer, Inc., *Developer Note: Macintosh Quadra 950*, Developer Technical Publications,
   1992 — §"Summary of features" p. 5; §"Faster video hardware" and §"Improved video on large
   monitors" p. 6; §"Identifying the Macintosh Quadra 950" (Gestalt machine type 26) p. 8.
4. Apple Computer, Inc., Macintosh Quadra 700 main-logic-board schematic — DAFB symbol ("DAFB II"),
   and the custom-IC signal wiring.
5. Apple Computer, Inc., Macintosh Quadra 900 production-validation (PVT) main-logic-board
   schematic — "Eclipse" project title block; dual-SCSI and VIA/interrupt wiring.
6. Macintosh Quadra 700/900 boot ROM, version $420DBFF3 (October 1991; also shipped in the
   PowerBook 140/170 family) — boot-time DAFB register access trace (Quadra 700 configuration,
   power-on through the first mode sets; all *observed* sequence data in §3.5, §4.2).
7. Apple Computer, Inc., *Designing Cards and Drivers for the Macintosh Family*, Developer
   Technical Publications — display connector sense-line codes and the extended-sense protocol.
8. National Semiconductor Corporation, *DP8531 Programmable Clock Generator* data sheet —
   synthesizer structure (reference divider, modulus, VCO, output divider); the register nibble
   packing in §2.5 is reconstruction, not from the sheet.
9. Macintosh Quadra 950 boot ROM, version $3DC27823 (March 1992) — the 950's separate ROM family;
   its video driver carries the AC842a/x555 programming path.
