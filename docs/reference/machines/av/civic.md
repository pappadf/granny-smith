# CIVIC and Sebastian — the AV graphics display path

**Contents:**

1. [Overview](#1-overview) — what the parts are, which machines carry them, the display path end to end, clocking
2. [Register file](#2-register-file) — the $50036000 serial window and the full register map, the VRAM windows, and Sebastian's $50F30800 file with the CLUT protocol
3. [Behaviour](#3-behaviour) — the timing core, the VBL and video-in interrupt pair, monitor sensing, the VRAM port and its two configurations, the capture datapath, Sebastian's mixing
4. [Programming model](#4-programming-model) — the declaration-ROM structure, the boot-time initialization sequence, the driver's open and control surface, depth switching, television output, and the video-in geometry the drivers program
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 What the parts are

The **Cyclone Integrated Video Interfaces Controller (CIVIC)**, Apple part **343S1096** (a later
revision carries the part number 343S1103), is "a CMOS chip in a 144-pin package" [1] p. 14 and the
frame-buffer heart of the AV Quadras. Apple's own function list for the part [1] p. 14:

- it "manages either 1 MB or 2 MB of video RAM (VRAM)"
- it "controls data transfers between VRAM and the Video Data Path Chip and between VRAM and the
  Sebastian video color palette chip"
- it "provides 32-bit or 64-bit data paths between VRAM and the main processor or a slot card;
  supports data bursts from the main processor in all transfer modes"
- it "performs convolution of graphics data for line-interlaced displays"
- it "provides NTSC and PAL timing signals"
- it "generates vertical blanking and video-in interrupt signals"

The glossary compresses the same part into one line: "A video control chip that manages VRAM,
generates video timing signals, and performs convolution where needed" [1] Glossary. CIVIC is
thus four things at once: the VRAM controller, the video-timing generator (pixel, line and frame
counters, the sync and blanking outputs), the arbiter between the graphics raster and the capture
raster that share the VRAM's serial port, and the source of the machine's two video interrupts.

**Sebastian**, Apple part **343S0704**, is "a video color palette and video digital-to-analog
converter (DAC) in a 100-pin CMOS chip" [1] p. 14, sitting immediately downstream of CIVIC. Its
documented capabilities [1] p. 14:

- it "accepts up to 64 bits of digital input, either as one 64-bit port or as one or two 32-bit
  ports"
- it "lets one 32-bit port handle digital video while the other processes graphics (including
  QuickTime), using the same or different color lookup tables"
- it "supports mixing video with still graphics, even with different color depths"
- it "supports both Truecolor and pseudocolor with alpha color lookup"
- it "supports a transparency effect when blending video with still graphics under the control of
  alpha bits"
- it "uses a convolution filter to minimize flicker in line-interlaced displays"
- it "supports displays with dot clocks up to 100 MHz"

Sebastian is the AV machines' RAMDAC/CLUT: it holds **two** independent color-lookup banks — one
for graphics, one for the captured video plane — mixes the two pixel streams into one digital RGB
stream, and converts the result to the analog RGB that leaves through the DB-15 monitor connector.
"Digital mixing" is the function Apple's own hardware-information string records for the part: it
is a "Color palette Digital to Analog converter for integrated video and graphics support" that
"provides digital mixing" [1] p. 14.

Neither chip has a published register-level data sheet; Apple's bibliography for the platform
names data sheets only for the two Philips capture chips, the "7169 Video Data Path Chip" and the
"7191B Digital Multistandard Decoder" [1] Bibliography. Every register fact on this page is
therefore reconstructed from the shipped software that drives the parts — the boot ROM's video
driver, its primary initializer and its video digitizer component — against the developer note's
functional description, with the reconstruction's residual holes listed in §6.

### 1.2 Machines that carry them

| Machine | Apple codename | VRAM as shipped | VRAM capacity | Pixel-clock chip |
|---|---|---|---|---|
| Macintosh Quadra 840AV | Cyclone (33 and 40 MHz builds) | two 512 KB banks | 1 MB, expandable to 2 MB by two more banks [1] pp. 30, 33 | Endeavor |
| Macintosh Centris 660AV | Tempest (25 and 33 MHz builds) | two 512 KB banks | 1 MB only [1] pp. 30, 33 | Clifton Plus, or a second variant the driver detects at run time |

The two boards are one design with speed variants [1] p. 10, and CIVIC/Sebastian are identical
across all four variants: the boot ROM carries a single video driver for both machines and
distinguishes them only by board speed (the clock-control registers of §2.3) and by which
pixel-clock chip it programs. The shipped ROM's declaration data encodes the pair directly: the
universal declaration ROM at ROM offsets $1D5068–$1FFFFF carries two board sResources — "Macintosh
3A" (spID $25, board ID $3D, the Cyclone record) and "Macintosh 3B" (spID $36, board ID $50, the
Tempest record) — and one shared CIVIC driver, primary initializer, timing table and gamma table
for both [2] (board records at ROM offsets $1FFDDA and $1EFD54; one driver copy at $1E4AD8,
confirmed by byte scan). The machine pages [Quadra 840AV](q840av.md) and [Centris 660AV](q660av.md)
and the family overview [AV](av.md) carry everything board-level; the capture chain upstream of
VRAM is [VDC](vdc.md)'s.

### 1.3 The display path end to end

The video output system, in the developer note's own block diagram [1] Figure 2-7 p. 31, is built
around two banks of VRAM controlled by CIVIC. The picture:

```
main processor ─┐                                   ┌─ DB-15 analog RGB ── monitor
PSC (I/O DMA)  ─┼─ CPU/video bus ─ CIVIC ─ VRAM ─ Sebastian ─┬─ Mickey ─ composite/S-video jacks
MUNI (NuBus)   ─┘                 (1 or 2 MB)       └─ (pixel stream tapped for TV encoding)
VDC (capture) ───────── capture writes ────┘
                 clocked by Endeavor/Clifton (dot clock) + 14.31818/17.734475 MHz (NTSC/PAL)
```

The VRAM's bus masters, in the developer note's enumeration: with the VRAM configured as a single
frame buffer, CIVIC "controls data access to VRAM from... the main processor... the PSC, using I/O
direct memory access... [and] the MUNI chip"; configured as two frame buffers, "it can also store
video data from the VDC in the video VRAM" [1] pp. 30–31. The internal video bus is "two sets of
32 lines each (or a combined set of 64 lines) for video or graphics" [1] p. 18 — the same 32/64
distinction the BusSize register of §2.3 controls. Data bursts from the main processor are
supported "in all transfer modes" [1] p. 14.

Both images exit VRAM "through its serial access memory port and pass to the Sebastian color
palette chip. Sebastian provides independent color lookup tables for video and graphics images and
mixes them into a single digital RGB data stream. The Sebastian then converts the result into
analog RGB video, using internal DAC circuits" [1] p. 31. From there "analog RGB data passes to
the Mickey encoder chip. Mickey either sends RGB directly to the monitor connector or encodes it
into NTSC or PAL video signals in composite or S-video format" [1] pp. 30–31 — the TV-out path,
which software reaches through exactly two bits (§4.6).

On the capture side, the VDC "scales down the video image and converts its format to either 8-bit
grayscale, 15-bit RGB, or 16-bit YUV. It stores the result in the VRAM buffer under the control of
the CIVIC chip" [1] p. 32 — into the capture frame buffer at **$50200800** (§2.5), which Sebastian
overlays onto the graphics raster. The video-in digitizer chain (DMSD, VDC, the Cuda-mediated I²C
bus that configures them) belongs to [VDC](vdc.md); this page covers CIVIC's half of the capture
path — the register surface, the interrupt and the frame-buffer geometry.

### 1.4 Clocking

CIVIC is a multi-clock part. Table 2-2 of the developer note gives it both processor-side clocks —
PClk at 80.0000/50.0000 MHz ("Main processor, MCA, CIVIC") and BClk at 40.0000/25.0000 MHz ("Main
processor, MCA, CIVIC, PSC, MUNI") [1] Table 2-2 p. 17 — and the pixel clock is synthesized
externally: "Dot clock — Various — Endeavor" with usage "Sebastian" [1] Table 2-2 p. 17. The
television frequencies are fixed oscillators: NTSC 14.31818 MHz and PAL 17.734475 MHz, both feeding
"Sebastian, Mickey" [1] Table 2-2 p. 17.

The pixel-clock synthesizer is itself programmable. The Quadra 840AV carries the **Endeavor** ("a
programmable video clock chip" [1] p. 17); "the equivalent in the Macintosh Centris 660AV is called
Clifton Plus" [1] p. 17. Both are programmed through three byte-wide registers at **$50F2E000** —
numerator, denominator and a clock-select byte — and the driver writes per-mode frequency words
taken from the same timing tables as CIVIC's own registers (§4.2). Only two references to the
synthesizer's base exist in the entire 2 MB ROM, both inside the video code: one in the boot-time
initializer, one in the driver's dynamic-switch path [2] (ROM offsets $1FF6CE and $1E5D7E). The
660AV variant is additionally probed at driver-open time: the driver shifts an ID word into the
same serial port and reads a byte back, distinguishing the Clifton Plus chip from a
second-generation variant that answers a read at all ones [2] — the two 660AV clock chips answer
differently, and the driver carries frequency words for both in every mode's parameter block
(§4.2).

Software never sees any of these clocks directly: it sees CIVIC's interrupt outputs (§3.2, §3.3)
and, indirectly, the timing that the mode tables encode (§4.2).

## 2. Register file

### 2.1 Base addresses and how software finds them

CIVIC's registers live at physical **$50036000**. The address is a constant of the platform, but
shipped software recovers it at run time from the machine's decoder-information record — the same
record the PSC's drivers use ([PSC](psc.md) §2.1) — and the boot ROM's own code uses **only** the
$50036000 form: a byte-pattern scan of the whole 2 MB image finds the alternative form $50F36000
referenced **nowhere**, while $50036000 and its per-register offsets appear throughout the video
code [2] (*observed* by scan). The two forms alias — the I/O region's decode reaches the same
register file through both the $5003xxxx serialized window and the $50F3xxxx I/O window — but the
exact alias rule (which address bits the decoder ignores) is not established (§6.14).

Two consequences of the address itself:

- **The registers are reachable only in 32-bit addressing mode.** $50036000 lies far above the
  24-bit logical map, so no 24-bit mapping can touch the window; every shipped access is a
  32-bit-mode longword access [2].
- **Sebastian's window is not adjacent.** Sebastian sits at its own base, **$50F30800** (§2.6), in
  the I/O region rather than the serialized video window — the ROM's video digitizer references
  $50F30800/$50F30810/$50F30820 directly, and no shipped code ever uses a $5003xxxx form for it
  [2] (ROM offsets $1AAB0E, $1AAB1A, $1AAB6C and the PCBR sites of §2.7).

The machine's video base addresses are all pinned by the ROM's per-machine records:
`VideoInfoCyclone` at ROM offset $152AC reads, in order, VRAM physical $50100000, VRAM logical-32
$50100000, **VRAM logical-24 $00000000** — i.e. there is deliberately no 24-bit alias of VRAM —
then the slot number alias $09, the slot-PRAM address $46, the super-directory sResource ID $7D and
the board sResource ID $25; `VideoInfoTempest` at $152BC is identical except for board ID $36 [2]
(*observed* in the ROM data).

### 2.2 The serial register protocol — one bit per longword

CIVIC's register interface is unlike anything else on the platform: **only bus bit D[0] of each
longword is meaningful, and each *bit* of a logical register occupies its own longword**, the
register's LSB at the lowest address and each subsequent bit four bytes higher. A 12-bit register
at offset $380 therefore occupies $380, $384, $388 … $3AC; a 1-bit register occupies exactly one
longword. The protocol is established identically by three independent shipped writers — the boot
ROM's video driver, its primary initializer and its video digitizer component [2]:

- **Write**, LSB to MSB ascending: for each bit of the value, store the longword at the current
  address, shift the value right by one, advance the address by 4.
- **Read**, MSB to LSB descending: start at `base + width × 4` and walk backwards, assembling the
  value one bit per longword.
- All accesses are longword-sized, in 32-bit addressing mode.

The video digitizer's copies of the two accessors are the cleanest witnesses: its bit-writer at
resource offset $57C0 stores one longword per bit with a `NOP` between stores (68040 store
synchronisation), and its bit-reader at $5810 walks `address + (nbits−1)×4` downward, shifting each
longword's bit 0 into place [2] (resource offsets $57C0, $5810 within the ROM-resident digitizer
at ROM file offset $1A5200). Every access the digitizer ever makes is a 1-bit access [2] —
multi-bit registers belong to the driver alone.

A model that serves the longword slots as plain storage — one stored bit per longword slot —
satisfies both this protocol and the direct accesses of §2.4.

### 2.3 The CIVIC register map

The map below is reconstructed from the shipped driver's own register-selection table — the table
the ROM's declaration-ROM copy of the driver carries, which pairs each logical register with its
offset and bit width — cross-checked against every register the boot initializer, the driver and
the digitizer touch [2]. The names are descriptive labels for this page; the offsets, widths and
polarities are the shipped software's, and where the shipped software disagrees with itself the
disagreement is called out. Offsets from $50036000:

| Offset | Width | Name | Meaning |
|---|---|---|---|
| +$000 | 1 | VBLInt | read-only VBL flag, **active high**: bit 0 = 1 means "vertical blank pending" (§3.2) |
| +$004 | 1 | Enable | enable the timing generator |
| +$008 | 1 | VDCInt | read-only video-in (VDC) field flag, **active low**: bit 0 = 0 means "field interrupt pending" (§3.3) |
| +$00C | 1 | VDCClr | acknowledge the VDC interrupt: write 0 then 1 (§3.3) |
| +$010 | 1 | VDCEnb | enable the VDC field interrupt |
| +$014 | 1 | VidInSize | capture row stride: 0 = 1024 bytes, 1 = 1536 bytes (§3.6) |
| +$018 | 1 | VDCClk | the VDC's VRAM-port clock gate — **1 = clock off** (freezes capture writes; §3.6) |
| +$01C | 1 | ScanCtl | scan mode: 0 = progressive, 1 = interlaced |
| +$020 | 3 | GSCDivide | graphics shift-clock divide ratio — depth-dependent (§3.1) |
| +$02C | 1 | VSCDivide | video-in shift-clock divide (§3.6) |
| +$040 | 1 | VRAMSize | installed VRAM: 0 = 1 MB, 1 = 2 MB |
| +$044 | 2 | RefreshCtl | VRAM refresh mode: 1 at 25 MHz, 2 at 33/40 MHz (§4.2) |
| +$04C | 1 | BusSize | **0 = 32-bit VRAM port (video-in shares it), 1 = 64-bit graphics-only** (§3.5) |
| +$050 | 1 | SpeedCtl | board speed class: 1 = 25 MHz, 0 = 33/40 MHz (§4.2) |
| +$054 | 1 | ConvEnb | convolution (flicker filter) enable |
| +$058 | 1 | SenseCtl | sense-line driver enable |
| +$05C | 1 | Sense0 | drive sense line C |
| +$060 | 1 | Sense1 | drive sense line B |
| +$064 | 1 | Sense2 | drive sense line A |
| +$068 | 1 | SenseTst | sense-line test / tristate |
| +$06C | 1 | SyncClr | **1 = disable the RGB sync outputs** (blanking / power save); reads **inverted** (§4.6) |
| +$080 | 3 | ReadSense | read the three sense lines, LSB first (§3.4) |
| +$08C | 8 | RowWords | active-video row length in longwords |
| +$0C0 | 9 | BaseAddr | active-video base: the byte offset right-shifted 5; the driver masks the value to $FF (§4.2) |
| +$100 | 1 | VBL test | VBL test/control strobe (part of a triple with +$104/+$108) |
| +$104 | 1 | VBL test | as above |
| +$108 | 1 | VBL test | as above |
| +$10C | 1 | Reset | reset the timing generator (§4.2) |
| +$110 | 1 | VBLEnb | enable the VBL interrupt |
| +$114 | 1 | H test | horizontal test/control strobe (triple with +$118/+$11C) |
| +$118 | 1 | H test | as above |
| +$11C | 1 | H test | as above |
| +$120 | 1 | VBLClr | acknowledge the VBL interrupt and re-arm: write 0 then 1 (§3.2) |
| +$124 | 1 | AdjF2 | timing adjust field 2 (§4.2) |
| +$128 | 1 | AdjF1 | timing adjust field 1 (§4.2) |
| +$12C | 1 | TestEnb | enable the timing core's test mode |
| +$140 | 12 | CntTest | count test — read three times as a settle delay; reads are **side-effect-free** (§3.4) |
| +$180 | 12 | HSerr | horizontal serration |
| +$1C0 | 12 | VInHAL | video-in horizontal active start (§3.6) |
| +$200 | 2 | VInHFPD | video-in early horizontal front-porch / blank delay — always programmed 0 by every shipped writer |
| +$208 | 1 | VInDoubleLine | capture-side field doubling (the later chip revision's feature): **1 = disabled**, 0 = enabled (§3.6) |
| +$240 | 12 | VInHFP | video-in horizontal front porch |
| +$280 | 12 | HlfLn | half line |
| +$2C0 | 8 | HEq | horizontal equalization |
| +$300 | 12 | HSP | horizontal sync pulse |
| +$340 | 12 | HBWay | horizontal "b-way" |
| +$380 | 12 | HAL | horizontal active-video start |
| +$3C0 | 12 | HFP | horizontal front porch |
| +$400 | 12 | HPix | horizontal total |
| +$440 | 10 | PipeD | pipeline delay — depth-dependent (§4.2) |
| +$480 | 12 | VHLine | vertical total, counted in **half-lines** (§3.1) |
| +$4C0 | 12 | VSync | vertical sync position |
| +$500 | 12 | VBPEq | vertical back-porch equalization |
| +$540 | 12 | VBP | vertical back porch |
| +$580 | 12 | VAL | vertical active-video start |
| +$5C0 | 12 | VInVAL | video-in vertical active start (§3.6) |
| +$600 | 12 | VInVFP | video-in vertical front porch |
| +$640 | 12 | VFP | vertical front porch |
| +$680 | 12 | VFPEq | vertical front-porch equalization |
| +$6C0 | 12 | CurLine | current scan line, read-only |

Multi-bit registers occupy consecutive longwords, so the offsets inside a width-*n* register run
from its base to `base + (n−1) × 4`; the MSB slot of the 3-bit ReadSense at $080 is the longword
at $088 — a fact worth stating because a naive reading of "one register per offset" would mistake
$088 for a separate register.

### 2.4 The five direct-access registers

Exactly five registers are also poked as single longwords, bypassing the serial protocol — all
five are 1-bit registers, so the direct write and the serial write of bit 0 land on the same
longword slot:

| Register | Offset | Direct accesses in shipped code |
|---|---|---|
| VBLInt | $000 | the VBL handler reads it as a flag test (§3.2) |
| SyncClr | $06C | the TV-out and power-save paths write it (§4.6) |
| Reset | $10C | the reset sequence writes it twenty times (§4.2) |
| BusSize | $04C | the digitizer reads it back on every call (§4.8) |
| VBLClr | $120 | the VBL acknowledge writes it twice (§3.2) |

The VDC acknowledge pair (+$00C) is likewise written as two direct longword writes (0 then 1) [2]
(the digitizer's interrupt service). A compatible part must serve **both** access styles for these
registers; nothing in the shipped software constrains whether a direct write to a multi-bit
register's base longword does anything at all (§6.13).

### 2.5 The VRAM windows

| Window | Address | Size | Notes |
|---|---|---|---|
| Graphics frame buffer | **$50100000** | 1 MB or 2 MB | physical = 32-bit logical; **no 24-bit alias** (`VideoInfo` carries $00000000 as the 24-bit base) [2] |
| Capture frame buffer | **$50200800** | mode-dependent | the VDC's continuous capture target, overlaid onto the graphics raster through Sebastian |

The graphics window's base and the absence of a 24-bit alias are ROM data facts (§2.1). Its size
is established at bring-up by a probe: the boot initializer writes the four-byte constant `'Nano'`
at `VRAM base + 2 MB − 8` and reads it back; a mismatch selects the 1 MB configuration [2] (the
`'Nano'` constant sits at ROM offset $1FF396, inside the initializer's code block; *observed* by
scan). The developer note corroborates the capacity story from the board side: "two banks of
VRAM... Each bank holds 512 KB and is expandable in the Macintosh Quadra 840AV to 1 MB. Thus, total
VRAM capacity in the Macintosh Quadra 840AV may be either 1 MB or 2 MB; in the Macintosh Centris
660AV it is limited to 1 MB" [1] p. 30, with expansion requiring 80-ns chips on the same SIMM
configuration as other Quadras [1] p. 34.

The capture buffer's address is the platform's video-in base constant: the 2 MB ROM references
$50200800 at six sites — four inside the video driver's video-in geometry code and two inside the
digitizer component, with the follow-on address $50200804 referenced by the digitizer's liveness
probe [2] (ROM offsets $1E560E, $1E57DC, $1E63FC, $1E6414, $1A6272, $1AB512, and $1AADDC for +$4;
*observed* by scan). Note the arithmetic: $50200800 is exactly VRAM + $100800 — the capture buffer
lives at an offset just past the 1 MB mark of the linear VRAM aperture, which is why its
coexistence with 1 MB-VRAM configurations is one of the platform's genuine open questions (§6.5).

Configured as one frame buffer, all VRAM belongs to graphics and "the video input can be disabled"
[1] p. 30. Configured as two, "the VRAM banks shown at the top of the figure can store video
frames and the lower banks can store only graphics" [1] p. 30 — the developer note's own drawing
places the capture plane in the upper banks.

### 2.6 Sebastian's register file

Sebastian's window sits at **$50F30800**, byte-wide registers on a $10 stride:

| Offset | Absolute | Name | Function |
|---|---|---|---|
| +$000 | $50F30800 | address register | CLUT/DAC address — the entry index (auto-incrementing, below) |
| +$010 | $50F30810 | data register | CLUT/DAC data — four bytes per entry: R, G, B, alpha |
| +$020 | $50F30820 | PCBR | Pixel Bus Control Register (§2.7) |
| +$030 | $50F30830 | — | whether a fourth register exists here is unknown; no shipped software touches it (§6.12) |

### 2.7 The Pixel Bus Control Register

PCBR is the one byte through which CIVIC tells Sebastian what the pixel stream looks like. Its bit
layout, fixed by the shipped tables that program it and the driver code that read-modify-writes it
[2]:

| Bit | Meaning |
|---|---|
| 2–0 | graphics depth code: 0 = 1 bpp, 1 = 2 bpp, 2 = 4 bpp, 3 = 8 bpp, 4 = 16 bpp, 5 = 32 bpp |
| 3 | video-in plane depth: 0 = 8 bpp grayscale, 1 = 16 bpp |
| 4 | video-in enable |
| 5 | convolution enable |
| 6 | CLUT bank select: 0 = graphics CLUT, 1 = video-in CLUT |
| 7 | overlay enable — blend the video-in plane over graphics |

The shipped timing tables carry one PCBR value per depth, and those values are the primary evidence
for bits 4 and 7: for the Hi-Res 640×480 2 MB display the table programs **16, 17, 18, 19, 20** for
1, 2, 4, 8 and 16 bpp respectively, and **5** for 32 bpp [2] (the $B1 parameter node at ROM offset
$1FD5EC; *observed* by dump, §4.3). Every one of the first five values has bit 4 set — nominally
"video-in enable" — even in plain graphics modes, while the 32 bpp value clears it. Either bit 4 is
dual-purpose (a pipeline mode for CLUT-indexed depths) or the depth-code reading of the low bits is
incomplete; the tables are authoritative for what must be written, the semantics are not
established (§6.10).

### 2.8 The CLUT access protocol

The CLUT is written through the address/data pair as **four bytes per entry: red, green, blue,
alpha** [2]:

1. select the graphics CLUT by clearing PCBR bit 6 (every shipped CLUT access does this first);
2. write the entry index to the address register;
3. write four bytes to the data register: R, G, B, alpha — the address **auto-increments after
   every fourth data byte**, so a whole block can stream after one index write (the driver uses
   the streaming form only at 8 and 16 bpp);
4. read back the same way for verification: the CLUT is readable [2].

Sub-8-bpp entries do not sit at CLUT indices 0 to 2^depth−1; they are spread across the 256-entry
bank by a fixed rule the driver encodes as per-depth constants — `entries = 2^depth`,
`start = 256/2^depth − 1`, `skip = 256/2^depth` [2]:

| Depth | Entries | First entry at | Spacing |
|---|---|---|---|
| 1 bpp | 2 | $7F | $80 (so the two entries are at $7F and $FF) |
| 2 bpp | 4 | $3F | $40 |
| 4 bpp | 16 | $0F | $10 |
| 8 bpp | 256 | $00 | $01 (identity) |
| 16 bpp | 32 | $00 | $01 (a 32-entry use whose datapath is undocumented, §6.11) |

The boot initializer's very first CLUT write demonstrates the 1-bpp rule: with the mini-gamma from
the per-monitor configuration table, it writes white at index $7F and black at index $FF [2]. At
open time the driver loads a full gamma table from the slot gamma sResource — the ROM's Hi-Res
display carries a 288-byte `'Mac Std Gamma'` block at ROM offset $1E49A4 [2] (*observed*) — falling
back to a linearly-generated table when the resource is absent.

Each entry carries the **alpha** byte, and the driver preserves it: with the video-in plane active
it read-modify-writes every CLUT entry it touches so that an existing alpha value survives [2]. The
alpha byte is per-CLUT-entry; the transparency the developer note describes ("a transparency
effect when blending video with still graphics under the control of alpha bits" [1] p. 14) is
per-pixel — the two alphas are different mechanisms whose interaction is undocumented (§3.7,
§6.9).

### 2.9 Reset state

The power-on values of the latches are unknown: no shipped software ever reads back a timing field,
a divide ratio or a control bit to observe a reset value, so nothing in the observed software
contract constrains them (§6.13). What is pinned is the *software* reset the driver performs —
twenty direct longword writes to the Reset register, five each of 0, 1, 0, 1 in turn, then Enable
set — re-establishing the timing core from the current mode's parameters (§4.2).

## 3. Behaviour

### 3.1 The timing core

CIVIC's timing core generates the whole display raster: pixel clock enable, horizontal and vertical
counters, sync and blanking placement, and the two interrupt outputs. Software programs it by
writing, per mode, one vertical group and one horizontal group of count fields (§2.3) plus
depth-dependent divider and pipeline values, then pulsing Reset and setting Enable.

The vertical fields are counted in **half-lines**: the worked example of §4.3 shows VHLine = 1050
for a mode whose total frame is 525 lines, VFP − VAL = 960 = 2 × 480 for an active height of 480
lines, and the interlaced modes' half-line fields (HlfLn, HEq, the equalization porches) exist to
place the interlace half-line offset. The horizontal fields are counted in units that scale with
depth — the 16 bpp values are exactly twice the 8 bpp values in the same mode, and the 1 bpp values
equal the 8 bpp values with a different shift-clock divide (GSCDivide = 3 at 1 bpp, 0 at 8/16 bpp)
— consistent with a shift-clock tick that moves a depth-dependent number of pixels per tick
(*inferred* from the shipped tables' arithmetic; the exact counting unit and edges are not
documented anywhere, §6.1).

The core is blanked while it is being reprogrammed: the mode-switch paths write SyncClr = 1 first
(disabling the sync outputs), reprogram, then re-enable — which is also the developer note's
power-saving contract, "disabling the sync outputs going to the monitor... While the sync outputs
are disabled, the monitor will show black" [1] Chapter 11 p. 411. The ScanCtl bit selects
progressive or interlaced scan per mode — 0 for the RGB monitors, 1 for the NTSC/PAL television
rasters [2] (per-mode values in the shipped parameter nodes).

### 3.2 The VBL interrupt

CIVIC "generates vertical blanking and video-in interrupt signals" [1] p. 14. The VBL interrupt is
the primary one: the driver installs a slot-0 service handler with the Slot Manager at the highest
service priority, enables VBLEnb, and thereafter every frame the flag latches, the line asserts,
and the handler runs [2]:

1. read VBLInt (bit 0; **active high** — bit 0 = 1 means pending);
2. if pending: write 0 to VBLClr, then write 1 — "clear this interrupt / re-enable VBLs" — then run
   the queued VBL task list and return *serviced*;
3. if not pending: return *not serviced* — the step matters, because the video-in handler shares
   the same line (§3.3) and the Slot Manager walks on to it.

The wait-for-vertical-blank primitive every mode-change path uses is the same contract wrapped in
a spin: raise the interrupt level to mask level-2 interrupts, write VBLClr 0 then 1, and spin
reading VBLInt bit 0 until it sets [2].

Delivery to the 68040 is through the PSC's pseudo-VIA2 window, the family's total interrupt path
([PSC](psc.md) §2.8): the vertical-blanking line is one of the active-low lines of the VIA2
window's slot/status byte at $50F03E00, dispatched by the ROM's level-2 slot-interrupt code as
slot 0's service queue [2]. The bit position within the $78 dispatch mask follows the classic
on-board-video position (bit 6) — the exact bit-to-slot mapping of that byte is *inferred*, and is
left so in [PSC](psc.md) §2.8. CIVIC's own handler runs first at its higher priority, and the
enable path is the classic one: the Slot Manager sets the slot-0 enable when the driver installs
its handler, and the driver keeps VBLEnb set for the life of the display.

### 3.3 The video-in (VDC) interrupt

The second interrupt is the capture field flag, and its polarity is **inverted relative to VBL**:
VDCInt bit 0 = **0** means "a field interrupt is pending" — active low, where VBLInt bit 0 = 1
means the same thing. The shipped digitizer's interrupt service is the byte-for-byte witness [2]
(within the ROM-resident digitizer at ROM file offset $1A5200):

```
LEA     $50036000,A0
MOVE.L  $8(A0),D0        ; read VDCInt
BTST    #$0,D0
BEQ.S   VdcPending       ; bit 0 clear -> it IS a VDC interrupt
```

One interrupt is generated per captured field. The acknowledge is two direct longword writes to
VDCClr — 0, then 1 [2]. The delivery path is the **same line as VBL**: the digitizer installs its
own slot-0 handler at a lower service priority than the driver's, so the driver's handler runs
first, finds VBLInt clear, returns *not serviced*, and the Slot Manager walks on to the digitizer's
handler [2]. The enable/acknowledge arming sequence the digitizer performs at open time, all
through the serial writer [2] (resource offset $5A50):

```
VDCEnb  := 0          ; mask
VDCClr  := 0          ; acknowledge any stale edge
VDCClr  := 1
<install slot-0 handler, lower priority than the driver's>
VDCEnb  := 1          ; unmask
```

and the disarm at close: VDCEnb := 0, VDCClr := 1, remove the handler [2] (resource offsets $5AE0,
$5B1A). The raster position of the interrupting edge, and whether CIVIC generates the edge itself
or merely latches a VDC output pin, are not established (§6.7).

### 3.4 Monitor sensing

CIVIC implements the Macintosh display connector's sense-line identification [1] p. 35 on behalf of
built-in video. The base model is the classic one: a DB-15 connector carrying "the red, blue, and
green video output signals and the composite sync signal along with the three sense lines that the
computer uses to determine the type of monitor that is connected" [4] Table 12-7 p. 420, with the
simple three-bit code table of the II family (portrait, two-page, 640×480, no monitor, and four
combinations "Reserved by Apple" [4] Table 12-2 p. 409). The AV machines extend the scheme in two
directions, both implemented in CIVIC's register file and both driven by the boot initializer
[2]:

**Indexed sensing.** With all sense outputs released, a read of ReadSense returns the monitor's
hard-wired three-bit code directly. The AV driver's monitor tables enumerate **fifteen** indexed
codes — the classic assignments (full-page mono, two-page, Hi-Res 640×480, no-connect) plus
fill-ins for the previously reserved combinations (21-inch RGB two-page, the 16-inch displays, NTSC
and PAL monitors, the RGB full-page) plus, beyond the three-bit space, multiscan classes reached
only through the extended probe [2] (the initializer's configuration table, indexed by sense
code, in the ROM's initializer block).

**Extended sensing.** When the idle code is one of the combinations the classic table reserves for
extension, the initializer runs a three-step probe: drive sense line A, read the other two lines;
drive line B, read the other two; drive line C, read the other two — assembling a six-bit code from
the three two-bit readings [2]. The drive sequence for each line is a fixed write pattern to
SenseCtl/Sense0–2 followed by a settle delay of three reads of the CntTest register — reads that
must be free of side effects, because the delay is their only purpose [2]. The idle/drive pokes are
the sense registers' entire software contract; SenseTst exists in the map and no shipped software
exercises it (§6.8).

The full detection flow, as the boot initializer implements it [2]:

1. reset the sense drivers and read the indexed code;
2. if the code is one of the extension combinations, run the extended probe for a six-bit code and
   map it through the initializer's no-connect table (the raw codes the table keys on are $00
   PAL-boxed, $14 NTSC, $17 VGA, $2D 16-inch/portrait-goldfish, $30 PAL, $3A 19-inch);
3. resolve to a monitor identity — including the multiscan disambiguation, where one indexed code
   means Hi-Res unless the extended probe also answered, in which case it means a multiscan class;
4. honor two PRAM overrides: a slot-PRAM bit can force composite output (NTSC, or PAL with a second
   bit) when no monitor answered, and a burn-in signature in the parameter RAM forces the
   factory-test display;
5. if nothing is sensed and no override applies, **disable built-in video entirely** — the mode
   list is pruned so no CIVIC sResource ever matches, and the machine boots headless or on a NuBus
   card's display.

The classic "no monitor connected" code of [4] Table 12-2 is thus not terminal on this platform: it
is the entry to the extended probe, and only its failure — with no PRAM override — kills built-in
video.

### 3.5 The VRAM port and its two configurations

The single most consequential bit in the file is **BusSize** (+$04C): it selects the VRAM port
width, and with it the whole display-path configuration — the developer note's one-frame-buffer /
two-frame-buffer distinction [1] pp. 30–31 mapped onto one bit:

| BusSize | Port | Configuration | Consequences |
|---|---|---|---|
| 1 | 64-bit | graphics only | full bandwidth for deep modes; **video-in impossible** |
| 0 | 32-bit | graphics + video-in share the port | the two-frame-buffer configuration of [1] Figure 2-7; video-in enabled |

The mapping is not inferred: a display mode is video-in-capable if and only if its shipped
parameter block carries BusSize = 0, and the deepest modes — 32 bpp at 2 MB — carry BusSize = 1
precisely where the developer note's Table 2-13 shows video-in unsupported ("n.a." in the
graphics/video column) [1] Table 2-13 pp. 33–34 [2] (the $B1 node of §4.3). Every depth-switch and
video-in-mode switch rewrites BusSize as part of the mode's parameter block, and the depth switch
reflects the bit into the driver's "video-in enabled" state flag [2].

In the 32-bit configuration the capture plane and the graphics plane share the VRAM's serial
access-memory port on their way to Sebastian, which "provides independent color lookup tables for
video and graphics images and mixes them into a single digital RGB data stream" [1] p. 31; in the
64-bit configuration the whole port serves graphics, matching the internal video bus's "two sets of
32 lines each (or a combined set of 64 lines)" [1] p. 18.

### 3.6 The capture datapath

The capture half of CIVIC is a small, self-contained state machine coordinated with the [VDC](vdc.md)
upstream and Sebastian downstream:

- **Arbitration.** The VDC writes captured fields into VRAM through CIVIC's port whenever the port
  is in the 32-bit configuration (§3.5) and the VDC's clock is running. The clock gate is VDCClk,
  with the file's recurring inverted polarity: **VDCClk = 1 stops capture writes**, and the
  digitizer's asynchronous-grab path asserts exactly that — "to ensure that the data in VRAM isn't
  overwritten with a new field in the event that we take too long" — freezing the buffer before it
  copies the field out, and re-enabling the clock afterwards [2].
- **Row stride.** Captured rows land at a stride of 1024 or 1536 bytes, selected by VidInSize: the
  driver sets the bit whenever the requested window's byte width exceeds 992 (496 pixels at 2 bytes
  per pixel) [2]; the digitizer's geometry path applies the identical threshold through the same
  register [3]. The 992-byte boundary exists so a wide captured row fits the VRAM timing — each
  scan line of a "big" window is written into 1.5 nominal lines of VRAM [2].
- **Window geometry.** The capture window is positioned over the graphics raster by four count
  registers — VInHAL, VInHFP (horizontal start and end) and VInVAL, VInVFP (vertical start and end)
  — programmed relative to the *graphics* timing already in force, with the vertical offsets doubled
  in progressive modes (half-line units) and halved-pixel horizontal offsets for 8-bpp video under
  sub-16-bpp graphics [2] (§4.7). When video-in is off, the boot initializer and the driver both
  **park the window outside the raster** — VInHAL = $FF0, VInHFPD = $000, VInHFP = $FF8,
  VInVAL = $FF0, VInVFP = $FF8 [2] — so an inactive capture plane generates no overlay pixels.
- **Field doubling.** VInDoubleLine (the later chip revision's feature) makes the hardware write
  each captured line twice on scanout; **1 disables it**. The driver clears it at bring-up, and the
  digitizer sets it only when it has decided to do line doubling in *software* instead — copying
  each captured row twice into the destination [2] (§4.8).
- **The interrupt.** One VDC interrupt per captured field (§3.3) is what tells software a frame is
  ready; the digitizer's field-parity logic counts on exactly one edge per field, using the count's
  low bit to alternate even/odd fields for callers that ask for both [2].

### 3.7 Sebastian's mixing and overlay

With the video-in plane enabled, Sebastian blends the captured plane over the graphics raster
inside the window CIVIC's VIn* registers place. The shipped software contract is the PCBR bit pair
**bit 4 (video-in enable) and bit 7 (overlay enable)**: the digitizer sets both to turn the overlay
on and clears bit 7 to turn it off, and the driver's mode switches preserve bits $88 (overlay +
video-in depth) across every rewrite [2].

The *keying* model is depth-dependent, and the digitizer reports it as such [2]:

- with **16 bpp graphics**, the overlay is alpha-keyed — each graphics pixel's 1-5-5-5 encoding
  carries the transparency bit in bit 15, exactly where the capture chip's chroma-keyer drives it
  onto the pixel bus, and the digitizer reports an alpha-type digitizer;
- with **8 bpp or less**, there is no alpha bit, so keying is by **key colour**: the graphics CLUT
  marks which indices the video plane replaces, and the digitizer reports a key-colour-type
  digitizer, maintaining a 256-byte key-colour map and a saved copy of the graphics CLUT to
  restore on close [2].

Apple ships the capture chip's keyer **disabled**, so the per-pixel alpha is identically zero in a
stock machine and the overlay's blending reduces to the key-colour path in practice [2] [3]
(*observed* in the shipped configuration writes). What Sebastian's own mixing arithmetic is —
whether the per-pixel alpha bit selects between the planes or scales a blend, and how the
per-CLUT-entry alpha byte of §2.8 relates to it — is not documented anywhere (§6.9). The developer
note's functional claim stands on its own: Sebastian "supports mixing video with still graphics,
even with different color depths" and "supports a transparency effect when blending video with
still graphics under the control of alpha bits" [1] p. 14.

Sebastian's DAC side descends from the Philips desktop-video output chain the handbook documents:
the reference graphics-environment pipeline runs the digitized stream through a colour-space
converter into "the SAA7169 Triple DAC for analog RGB output conversion", with SAA7169 described in
the product index as a "35 MHz triple 9-bit D/A converter for high-speed video" [5]. Sebastian plays
exactly that role — palette plus triple DAC feeding both the RGB connector and the composite
encoder — at Apple's required dot clocks, which reach 99.958 MHz for the 1152×870 two-page display
[1] Table 2-14 pp. 36–37, within Sebastian's documented "dot clocks up to 100 MHz" [1] p. 14. The
same handbook identifies the capture-side "Video Data Path Chip" as a digital video scaler
(SAA7186-class), which is [VDC](vdc.md)'s story.

## 4. Programming model

### 4.1 Where the software lives

All three CIVIC/Sebastian drivers ship **inside the boot ROM**, as part of the universal
declaration ROM that occupies the top 176 024 bytes of the 2 MB image, ROM offsets $1D5068–$1FFFFF
[2]:

| Structure | ROM offset | Notes |
|---|---|---|
| Extended format block | $1FFFD8 | CRC $2D406934, independently recomputed and verified; byteLanes $0F; revision 1, Apple format [2] |
| vendorInfo | $1FFF4A | "Apple Computer, Inc." / "Macintosh CPU Family 5.0" / "Universal DeclROM" / **"Friday, June 25, 1993"** [2] |
| superDirectory | $1FFF94 | four directories; the CIVIC display directory is sResource ID $7D at $1FFE0A [2] |
| CIVIC video driver | **$1E4AD8** | one 'scod' block, 9412 bytes; DRVR `.Display_Video_Apple_Civic`, DRVR image $1E4ADC–$1E6F9B; header entry points open $409E4D40, control $409E4FAE, status $409E621A, close $409E61EC; driver flags $4C00, version 1 [2] |
| sRsrcType (shared by all video sRsrcs) | $1E6FBE | category 3 (display), cType 1 (video), Apple, **drHW $25** [2] |
| sRsrcName (shared) | $1E6FA4 | "Display_Video_Apple_Civic" [2] |
| CIVIC PrimaryInit sExec | **$1FEE70** | shared by both boards; 3796-byte block, code at $1FF1EC [2] |
| board sRsrc "Macintosh 3A" (Cyclone) | $1FFDDA | spID $25, board ID $3D [2] |
| board sRsrc "Macintosh 3B" (Tempest) | $1EFD54 | spID $36, board ID $50 [2] |
| sVidParmDir (parameter directory) | $1FEA02 | 52 per-mode parameter nodes at $1FC0E8–$1FEA01, in descending address order as the mode ID ascends [2] |
| CivicModeList | $1FFBC0 | the mode roster: 52 mode IDs [2] |
| boot PICTs | $1EFD84 / $1E6FFA | the Cyclone and Tempest start-up pictures (50 020 / 36 164 bytes) [2] |
| gamma table (Hi-Res example) | $1E49A4 | 288-byte 'Mac Std Gamma' [2] |
| video digitizer component | $1A5200 | the ROM-resident 'code' resource for the digitizer (§4.8), separate from the display driver [2] |

Only **one copy of the driver exists in the ROM**, and both boards' sResources point at it [2]
(*observed*). The video-in digitizer component also exists in a later build in System Enabler 088,
whose differences matter below [3].

The shipped roster — the mode IDs the ROM's mode list actually carries, extracted from ROM offset
$1FFBC0 [2] (*observed* by dump):

```
$80 $81 $84 $85 $88 $89 $8A $8B $8D $91 $92 $93 $94 $97 $98 $9B $9C $9D $9E $9F
$A2 $A3 $A4 $AA $AB $B0 $B1 $B2 $B3 $B8 $B9 $BA $BB $C2 $C3 $C7 $C8 $CC $CD $D1
$D2 $D6 $D7 $DB $DC $DD $DE $DF $E0 $E1 $E4 $E5
```

— 52 mode IDs covering the Apple monitor line (21-inch RGB two-page, full-page mono, the 16-inch
displays, two-page, Hi-Res 640×480 with its 512×384 and 640×400 family members, VGA, the 56/72 Hz
800×600 and 60/70 Hz 1024×768 multiscans, the 16-inch GoldFish, the 19-inch), the NTSC and PAL
television formats with their underscan/overscan variants, and the composite convolution modes —
matching the monitor rows of the developer note's Table 2-13 [1] pp. 33–34. Several IDs alias the
same parameter node (the directory maps 57 IDs onto 39 distinct records) [2].

### 4.2 The boot-time initialization sequence

The primary initializer runs at Slot Manager bring-up on slot 0, before any display is opened, and
performs the complete hardware bring-up [2] (the sExec code at $1FF1EC). In order:

1. **Read and initialize slot PRAM** for slot 0; on a virgin record, seed it. Prune the board
   sResource to the matching board record ($25 Cyclone / $36 Tempest) from the machine's box flag,
   via the initializer's two-entry board table [2] (at $1FFAC0).
2. **Fetch the base addresses** from the machine's video-info record: CIVIC at $50036000, Sebastian
   at $50F30800, VRAM at $50100000 [2].
3. **Quiesce the part**: Enable = 0, VBLEnb = 0, VDCEnb = 0, VDCClk = 1 — timing generator off, both
   interrupt sources masked, capture clock stopped [2].
4. **Set the speed class**: on the 25 MHz board, SpeedCtl = 1 and RefreshCtl = 1; on the 33/40 MHz
   boards, SpeedCtl = 0 and RefreshCtl = 2 [2].
5. **Clear the auxiliaries**: ConvEnb = 0, BusSize = 1 (graphics-only while programming), the six
   test strobes = 1, VInDoubleLine = 0 [2].
6. **Size the VRAM** with the `'Nano'` probe (§2.5) [2].
7. **Sense the monitor** (§3.4) and index the per-monitor configuration table with the monitor
   code and the VRAM size: the result is the default mode ID and its default depth [2].
8. **Update slot PRAM** with the sensed monitor identity and the default configuration [2].
9. **Prune the mode list** to the selected display's family, re-inserting the other family members
   as disabled entries [2].
10. **Fetch the mode's parameter block** from the sVidParmDir node (§4.3), and compute the
    active-video base: the display's base offset right-shifted by 5, masked to $FF [2].
11. **Blank the outputs**: SyncClr = 1; if the mode is video-in-capable, BusSize = 0; write ScanCtl
    and ConvEnb from the node [2].
12. **Program the pixel-clock synthesizer** — on the 840AV a reference-select longword into the
    memory-map controller followed by the node's numerator/denominator bytes and clock-A select; on
    the 660AV the bit-serial shifting of the node's frequency word with its control words and a PLL
    lock delay (§1.4) [2].
13. **Program the timing core** — vertical group (VHLine, VSync, VBPEq, VBP, VAL, VFP, VFPEq),
    horizontal group (HSerr, HlfLn, HEq, HSP, HBWay, HAL, HFP, HPix) — then **park the capture window
    out of the raster** (VInHAL = $FF0, VInHFPD = $000, VInHFP = $FF8, VInVAL = $FF0, VInVFP = $FF8),
    then GSCDivide, BaseAddr, RowWords, the two adjust fields, PipeD [2]. The initializer writes the
    adjust fields **deliberately interchanged** — the node's second field to AdjF1 and the node's
    first field to AdjF2 — and the driver later writes them the other way around; both orders work
    (§5).
14. **Program Sebastian**: the node's PCBR value, then the initial 1-bpp CLUT — white at $7F, black
    at $FF from the configuration table's mini-gamma (§2.8) [2].
15. **Paint the boot pattern**: the standard gray dither into VRAM, inverted on alternate rows, with
    the row length and row count taken from the node's geometry [2].
16. **Restart the timing core**: pulse the Reset register (twenty direct longword writes: five 0s,
    five 1s, five 0s, five 1s), then Enable = 1 [2].
17. **Choose the output route**: if PRAM says boot-on-television, set the memory-map controller's
    bypass bit and leave SyncClr = 1 (composite route); otherwise clear both (RGB route) [2] (§4.6).

The initializer never enables video-in: VDCEnb stays 0 and the capture window stays parked, so the
2 MB ROM boots every machine with the capture plane inert [2] (*observed* across full boots — the
boot path issues no VDC-interrupt-enable write, and the level-2 dispatch runs only the VBL
handlers).

### 4.3 The mode parameter block

Every mode's node — a `blocksize` longword, a 40-byte header, then one 30-byte block per depth —
encodes everything steps 12–14 need [2]. The node for the Hi-Res 640×480 display with 2 MB VRAM
(mode ID $B1, ROM offset $1FD5EC; *observed* by dump, all fields big-endian) [2]:

| Field group | Values |
|---|---|
| depth limits / flags | graphics up to 32 bpp; video-in up to 16 bpp; video-in-capable flag set; convolution off |
| equivalent TV modes | NTSC underscan $A2 / overscan $A3; PAL $BA/$BB; convolved NTSC $80, convolved PAL $85 |
| pixel-clock words | Endeavor M = 13, N = 61, clock A; Clifton frequency word $21C900, 23 bits, clock 1; the 660AV second-variant word $114890, 22 bits, clock 1 |
| scan | progressive |
| vertical | VHLine 1050 · VSync 1048 · VBPEq 4 · VBP 43 · VAL 82 · VFP 1042 · VFPEq 1045 · active rows 479 |
| per-depth blocks | 1 bpp: BusSize 0, GSCDivide 3, RowWords 32, PipeD 72, PCBR 16, H = 400·216·16·431·31·73·393·430 (HSerr·HlfLn·HEq·HSP·HBWay·HAL·HFP·HPix) · 8 bpp: BusSize 0, GSCDivide 0, RowWords 32, PipeD 72, PCBR 19, same H · 16 bpp: BusSize 0, GSCDivide 0, RowWords 64, PipeD 146, PCBR 20, H = 800·432·32·863·63·147·787·862 · 32 bpp: BusSize 1, GSCDivide 0, RowWords 80, PipeD 146, PCBR 5, H as 16 bpp |

Three properties of the node are worth pulling out. The **32 bpp block carries BusSize = 1** — the
one depth of this display that cannot host video-in, exactly as Table 2-13's "n.a." predicts [1]
p. 33. The **16 bpp horizontal values are exactly twice the 8 bpp values**, the concrete witness
for the depth-scaled counting unit of §3.1. And the 1 MB twin of this mode (ID $B0, node at
$1FD6CE) carries an identical header but a different 16 bpp block — BusSize 1 and the 1×
horizontal values — so the two VRAM sizes of the same monitor are genuinely different parameter
nodes, and a node's block count must not be read as the mode's depth list (the 1 MB node keeps a
trailing 32 bpp block that exists only to hold the doubled timing for video-in use) [2]
(*observed* by dump).

The QuickDraw-facing geometry comes from the sResource around the node: for the same $B1 mode, the
8 bpp video parameter record carries base offset 0, rowBytes 1024, bounds 640×480 at 72 dpi, and
the 32 bpp record carries base offset −512 ($FFFFFE00) with rowBytes 2560 — the per-depth base
shift that lets deep modes fit the same aperture [2] (*observed* in the sResource dump).

### 4.4 Driver open and state re-derivation

The driver's open path is notable for what it does **not** do: it never re-senses the monitor [2].
PrimaryInit already sensed and recorded the configuration in slot PRAM, so open *reads hardware
state back* instead:

- the driver allocates its private globals (84 bytes), caches the CIVIC, Sebastian,
  memory-map-controller and VRAM bases, the machine's box flag, and the saved monitor ID from slot
  PRAM;
- installs the slot-0 VBL handler (§3.2) and sets VBLEnb = 1, VBLClr = 1;
- loads the gamma table from the slot gamma sResource, or builds a linear one;
- fetches the mode's parameter block and walks the mode list to establish the current mode's base
  offset;
- **reads BusSize back**: bit 0 = 0 means video-in is enabled in the current mode — this becomes
  the driver's video-in state flag;
- **reads SyncClr back**: bit 0 = 0 (the register reads inverted, §2.3) means the composite route is
  active; with it, the convolution and television flags come from the parameter block and PRAM;
- serial-reads VRAMSize and the VDC gate states to complete the saved configuration;
- sets the gray/monochrome display flags for monochrome monitors.

The pattern matters for compatibility: a machine whose register read-back does not faithfully return
the last written value cannot boot this driver — the open path's conclusions about video-in,
television output and VRAM size all come from reading the part back, not from saved software state
[2].

### 4.5 Control and status surface

The driver implements the standard video-driver control and status codes — entry, gamma, gray page,
interrupt control, mode switch, default mode — plus the platform's published additions: the developer
note documents csCode 11 as **SetSyncs/GetSyncs**, "a csMode value of 0 enables the sync outputs,
and a csMode value of nonzero disables the sync outputs... While the sync outputs are disabled, the
monitor will show black" [1] Chapter 11 p. 411, which the driver implements by writing SyncClr = 1 on
nonzero and SyncClr = 0 on zero [2].

On top of the standard codes the driver carries a private block at $80–$8B, established by its
dispatch table [2]:

| Selector | Control | Status |
|---|---|---|
| $83 | SetAltSense — override the sensed monitor from PRAM | GetAltSense |
| $85 | SetCompOut — set the boot-on-television PRAM bits | GetCompOut |
| $87 | SetRGBByPass — switch the RGB/composite output route (§4.6) | GetRGBByPass |
| $88 | — | GetVideoIn — the video-in enable state |
| $89 | SetVidInMode — the video-in capture depth (§4.7) | GetVidInMode |
| $8A | SetVidInRect — the video-in window geometry (§4.7) | GetVidInRect |
| $8B | — | GetCompCapabilities — the television formats the current display can produce |

### 4.6 Television output and the sync outputs

The RGB-to-television switch is two bits deep, because the composite encoder (Mickey) is a
hardware-only part with no register file of its own — it "accepts analog video signals from the
Sebastian video color palette chip" and "encodes to NTSC or PAL... produc[ing] S-video, composite,
and RGB video outputs" [1] p. 15:

1. a longword write to the memory-map controller's **bypass register** at $50F30400 + $4C — 0
   routes the pixel stream to the DB-15 RGB port, −1 routes it to the composite encoder [2];
2. a write to CIVIC's **SyncClr** — 1 disables the RGB sync outputs while the television route is
   active [2].

The switch is only offered when the current display can produce a television raster: the driver
picks the equivalent NTSC or PAL mode from the current display's parameter header — the
equivalent-TV fields of §4.3's example — based on the requested standard, the convolution setting
and the installed VRAM size, performs a full dynamic mode switch to it, and then writes the two
bits, preserving the overlay and video-in-depth bits of PCBR and the VDC gate state across the
switch; if the display is already interlaced the driver takes a short path that switches the two
bits alone [2]. The developer note publishes the user-facing rules: NTSC underscan is 512×384 and
overscan 640×480, PAL underscan 640×480 and overscan 768×576; Apple convolution "is not supported
in more than 256 colors or when a video input window is active"; the switch is offered only from
512×384, 640×480 or 768×576 displays; a machine with no monitor boots on television if the PRAM bit
is set, or when Command-Option-T-V is held at start-up (the machine plays its start-up beep twice;
the user releases the keys at the second beep) [1] Chapter 11 p. 410.

### 4.7 The video-in geometry surface

Two private control calls drive the capture geometry, and both derive every register value from the
*graphics* timing already in force [2]:

**SetVidInMode** selects the capture depth and reprograms the shared clocking: for 16 bpp
video-in, VSCDivide = 0 and the horizontal timing is taken from the mode's 16 bpp parameter block
(or the 32 bpp block, which carries the doubled values, when the 16 bpp block is not
video-in-capable); for 8 bpp video-in, VSCDivide is set only when the *graphics* depth is 16 bpp,
and the horizontal timing comes from the graphics depth's own block. The routine then forces
BusSize = 0 — the parameter block's own value is deliberately overridden — and reprograms the eight
horizontal counts, the dividers, the base and pipeline fields, under the vertical-blank wait of
§3.2 [2].

**SetVidInRect** positions the window. The vertical arithmetic depends on the scan mode —
progressive: `VInVAL = VAL + 2·top`, `VInVFP = VAL + 2·bottom`; interlaced: the same sums rounded
down to even — and the horizontal arithmetic depends on both depths: at 16 bpp video-in (or 8 bpp
under 16 bpp graphics), `VInHAL = HAL + left − 1`, `VInHFP = HAL + right − 1`; at 8 bpp video-in
under sub-16-bpp graphics the offsets are halved — `VInHAL = HAL + (left>>1) − 1` — with the −1
the driver's own "fudge factor" [2]. The stride bit follows: `VidInSize = 1` when the window's byte
width exceeds 992 (§3.6). The end positions are clipped against the graphics porches, and an
illegal rect is rejected with a corrected rect returned to the caller — negative coordinates yield
a −1 rect, inverted rects a zero rect, and 8-bpp video-in rectangles have their left edge forced
even and width forced to a multiple of four [2]. All writes happen under the vertical-blank wait,
in the fixed order VidInSize, VInHFP, VInHFPD, VInHAL, VInVFP, VInVAL [2].

The driver's per-display capability table bounds what can be requested: indexed by monitor code,
VRAM size and capture depth, it gives the maximum window per combination — zero where the depth is
unsupported, and a "supported but height-limited" marker on 1 MB machines capturing 16 bpp, where
the driver computes the height as 512 pixels for widths up to 512 and 340 above that, a hard
~512 KB capture budget in the one-megabyte part [2]. The developer note corroborates the ceiling:
"the maximum video window size on the Macintosh Quadra 840AV is 640 by 480 pixels; the maximum
video window size on the Macintosh Centris 660AV is 512 by 384 pixels" [1] p. 33.

### 4.8 The digitizer's use of CIVIC

The video digitizer component — the QuickTime-facing digitizer that applications open to capture
video — is a separate program from the display driver, and its relationship to CIVIC is almost
entirely read-only and single-bit [2]:

- **The TV-mode flag.** The digitizer caches one bit — CIVIC's BusSize, inverted — as its
  "television mode" flag, and **resynchronizes it at the top of every call** by reading BusSize
  back: flag set means BusSize = 0, the 32-bit port, video-in enabled. The flag gates everything
  else: with it clear, the digitizer refuses to start the capture clock, reports no digitizer
  device, and skips the field-arrival probe [2] (resource offsets $00BA–$0102 for the resync).
- **A safety interlock.** The digitizer's bit-writer refuses one specific write: a request to clear
  VDCClk (start the capture clock) is discarded while the TV-mode flag is clear — software cannot
  start the capture plane while the port is in the graphics-only configuration [2] (the writer at
  resource offset $57C0).
- **Arming and servicing.** Open arms the VDC interrupt (§3.3's sequence), reads VRAMSize, clears
  VInDoubleLine, and installs the field handler; the field interrupt service reads VDCInt,
  acknowledges through VDCClr, freezes the buffer with VDCClk = 1 when an asynchronous grab is
  pending, and defers the copy; the deferred copy reads VidInSize to derive the source stride
  (1024 or 1536 bytes), copies line by line from the capture base $50200800 to the destination
  buffer, writing each source line twice when software line doubling is on [2].
- **Line doubling.** Two mechanisms cooperate: the digitizer decides, from the requested height and
  the VRAM budget, whether to capture fields and double lines in software (each captured line
  copied to two destination rows), and it then *disables* the hardware's own doubling
  (VInDoubleLine = 1) so the two doublers do not stack; with doubling off it re-enables the
  hardware bit (VInDoubleLine = 0) [2] (resource offsets $0C8E, $28F6). In a stock boot the
  geometry is 1:1 — neither doubling mechanism runs (*observed*).
- **Liveness probing.** After arming, the digitizer waits up to one second (60 ticks) for the first
  field interrupt; the System Enabler 088 build strengthens this into a true hardware liveness
  test: it stamps the magic longword $0001FEFF at **$50200804** (capture base + 4), starts the
  capture clock, waits one field, and fails if the pattern survived — the capture engine must
  demonstrably write into the buffer even with no video source attached, and the check's stride is
  taken from CIVIC's VidInSize [3].

The digitizer never touches a multi-bit CIVIC register — every access is a 1-bit read or write —
which is why the geometry registers belong to the display driver's two control calls and not to the
capture API [2].

## 5. Quirks & errata

- **One bit per longword.** Only D[0] of each longword slot is meaningful; a 12-bit register
  occupies 13 consecutive longwords stride 4, LSB first (§2.2). A byte- or word-sized model of the
  window breaks every access the shipped software makes.
- **Two access styles must both work.** Five registers are also poked as single direct longwords,
  and the VDC acknowledge is a pair of direct writes (§2.4); the digitizer's BusSize read-back
  exercises the direct style on every single call.
- **The two video interrupts have opposite polarities.** VBLInt is active high (bit 0 = 1 =
  pending); VDCInt is active low (bit 0 = 0 = pending) (§3.2, §3.3). The shipped polarity test was
  itself a late correction — the digitizer's service routine documents the low-active reading as
  "per new information" [2].
- **Three registers invert.** SyncClr reads inverted (open treats bit 0 = 0 as "driving
  composite"); VDCClk is 1 = clock off; VInDoubleLine is 1 = disabled (§2.3, §3.6). One chip
  away, PCBR's bit-4 story adds a fourth inverted convention (§2.7).
- **The acknowledge is a 0-then-1 pair, everywhere.** VBLClr and VDCClr both take two writes — 0
  then 1 — to clear and re-arm; a write-1-to-clear model never re-arms the interrupt (§3.2, §3.3).
- **AdjF1 and AdjF2 are written swapped by the boot initializer and unswapped by the driver** — and
  both orders work on real hardware (§4.2). Whichever way a model wires them, both orderings must
  produce a working display.
- **The mode data carries deliberately misleading corners.** The 1 MB Hi-Res node keeps a 32 bpp
  block that exists only to hold doubled timing for video-in; block count is not the depth list
  (§4.3). The 16 bpp horizontal counts are exactly double the 8 bpp counts in the same mode.
- **The register file is 32-bit-addressing-only** — it sits above the 24-bit map — and the boot ROM
  uses the $50036000 form exclusively, never $50F36000 (§2.1).
- **VRAM has no 24-bit alias** — the machine record's 24-bit VRAM base is a deliberate zero (§2.1)
  — and is sized at boot by a `'Nano'` magic write at base + 2 MB − 8 that must read back (§2.5).
- **The capture base sits inside the second VRAM megabyte**, at VRAM + $100800, yet 1 MB machines
  support capture; the partition is an open question, not a settled decode (§2.5, §6.5).
- **BaseAddr is 9 bits, and the driver writes only 8** — the value is masked to $FF before the
  shift arithmetic (§2.3).
- **CntTest reads are a settle delay** — three reads, side-effect-free by contract; the sense drive
  sequences depend on them (§3.4).
- **A mode's video-in capability is exactly its BusSize bit** — 32 bpp at 2 MB is
  graphics-only-by-table, and there is no Gestalt selector for video-in: the only ways to discover
  it are the driver's private status calls or reading BusSize back (§3.5, §4.8).
- **The boot never enables video-in.** VDCEnb stays 0, the capture window is parked outside the
  raster, and VDCInt must idle at 1 (its "no interrupt pending" value) or the level-2 dispatch
  will hand an unserviced interrupt to the Slot Manager (§3.3, §4.2).
- **The VBL line is shared.** The driver's handler and the digitizer's field handler ride the same
  VIA2 line and the same slot-0 queue, separated only by service priority and the
  serviced/not-serviced return (§3.2, §3.3).
- **CLUT entries are RGBA, spread across the bank at sub-8-bpp depths** — entries at $7F and $FF
  for 1 bpp — and the alpha byte must survive driver rewrites (§2.8).
- **PCBR values in the shipped tables set bit 4 in every CLUT-indexed mode** — including pure
  graphics modes — and clear it only at 32 bpp; the tables are the contract, the bit's name is not
  (§2.7).
- **Headless boot is a real state.** No monitor, no PRAM overrides, no burn-in signature: the mode
  list is pruned so CIVIC never matches, and built-in video stays off (§3.4).

## 6. Open questions

1. **The timing fields' counting unit.** The vertical counts are demonstrably in half-lines
   (VHLine = 2 × 525, active = 2 × 480 lines) and the horizontal counts scale with depth (16 bpp
   exactly double 8 bpp) — but what the hardware counts per tick, on which clock edge, and how
   GSCDivide maps depths to shift ratios, is documented nowhere (§3.1). The shipped values are
   load-bearing per-mode signatures; an implementation can treat them as opaque and derive geometry
   from the known modes, but a from-scratch timing generator cannot be written from this page
   alone.
2. **The function of AdjF1/AdjF2** — two single-bit timing-adjust fields, written swapped by the
   boot initializer and unswapped by the driver, both orders working (§4.2). What they adjust, and
   why the swap is harmless, is unknown.
3. **The convolution engine.** ConvEnb is one bit; the developer note adds the gating rules (not
   above 256 colors, not with a video-in window active [1] p. 410) but the filter kernel, its VRAM
   bandwidth cost and its interaction with depth are not described anywhere.
4. **The VRAM organization**: banking, the serial-access-memory port's transfer timing, and how
   the 32-bit and 64-bit port configurations change the refresh and burst behavior. RefreshCtl's
   two modes (1 at 25 MHz, 2 at 33/40 MHz) are only known as values (§2.3).
5. **How the graphics and capture planes partition one VRAM array.** The capture base is VRAM +
   $100800 — inside the second megabyte of the 2 MB aperture — yet 1 MB configurations support
   capture with meaningful maximum rectangles. Either the 1 MB decode aliases address bit 20, or the
   32-bit port configuration splits the array into a graphics half and a video-in half that appears
   past the 1 MB mark in the linear view. No document resolves it (§2.5).
6. **CIVIC I versus CIVIC II.** VInDoubleLine is the later revision's feature (§3.6); what else
   changed between 343S1096 and 343S1103, and which revision shipped in which machine, is unknown.
7. **The VDC interrupt's edge.** One interrupt per field is pinned by the field-parity logic
   (§3.3), but the raster position of the edge, and whether CIVIC generates it or merely latches a
   VDC output pin, are not established.
8. **The test machinery** — TestEnb, CntTest's twelve bits, the six VBL/H test strobes, SenseTst —
   no shipped software exercises any of it beyond the side-effect-free CntTest delay (§2.3, §3.4).
9. **Sebastian's mixing arithmetic** — whether the per-pixel alpha bit selects between the planes
   or scales a blend; how the per-CLUT-entry alpha byte relates to it; and what the key-colour
   datapath does below 8 bpp at the pixel level (§3.7).
10. **PCBR bit 4's double life** — set in every shipped CLUT-indexed mode including pure graphics
    ones, clear only at 32 bpp (§2.7). Either it is dual-purpose (a pipeline/indexed-mode control)
    or the "video-in enable" reading of the bit is simply wrong for this part.
11. **Sebastian's 32-entry CLUT use at 16 bpp** — presumably a per-channel 5-bit gamma lookup, but
    the datapath is not described (§2.8).
12. **Whether Sebastian has a fourth register at +$30** — the three-register file of §2.6 is
    complete for all shipped software; nothing establishes what, if anything, decodes there.
13. **Reset values of every latch in both files**, and whether a direct write to a multi-bit
    register's base longword does anything — no shipped software reads back any field before
    writing it (§2.4, §2.9).
14. **The $5003xxxx/$50F3xxxx alias rule** — that both forms reach the same CIVIC register file is
    established (§2.1); which address bits the decode ignores, and whether the same rule covers
    Sebastian's window or VRAM, is not.
15. **The extended-sense paths for the two-page and RGB full-page codes** — the shipped boot
    initializer explicitly stubs them ("support these later"); actual hardware behavior for those
    monitors through the extended probe is unverified (§3.4).

## References

1. Apple Computer, Inc., *Developer Note: Macintosh Quadra 840AV and Macintosh Centris 660AV
   Computers*, Developer Press, 1993 — §"Summary of Features" p. 5 (the video-input frame buffer
   "separate from main memory"); Chapter 2: §"Cyclone Integrated Video Interfaces Controller"
   p. 14 (function list; NTSC/PAL timing; the two interrupts), §"Sebastian" p. 14 (capabilities;
   "provides digital mixing"), §"Video Data Path Chip" p. 15, §"Mickey" p. 15, §"Endeavor" p. 17
   (programmable video clock chip; Clifton Plus), §"Digital Multistandard Decoder" p. 17, §"System
   Clocks" Table 2-2 p. 17 (PClk 80/50; BClk 40/25; dot clock from Endeavor to Sebastian; NTSC
   14.31818 and PAL 17.734475 to Sebastian and Mickey), §"Signal Buses" p. 18 (the video bus: two
   sets of 32 lines or one of 64), §"Video and Graphics I/O" pp. 30–31 (Figure 2-7 block diagram
   p. 31; the two VRAM configurations and their masters; the Sebastian mixing description),
   §"External Video Input" p. 32 (Figure 2-8; VDC scaling and formats), §"Video RAM Usage"
   pp. 33–34 (bank sizes; 840AV 2 MB / 660AV 1 MB; maximum video window 640×480 vs 512×384; Table
   2-13 VRAM sizes and monitor color depths; 80-ns expansion chips), §"Video Monitor Interface"
   p. 35 (DB-15; Technical Notes 144 and 326), §"Video Output Timing" pp. 36–37 (Table 2-14 Apple
   monitor timing values, dot clocks 30.240–99.958 MHz); Chapter 11: §"Video Television Output"
   p. 410 (underscan/overscan resolutions; convolution limits; boot-on-television;
   Command-Option-T-V), §"New Control and Status Routines" p. 411 (SetSyncs/GetSyncs, csCode 11);
   Bibliography (Phillips *7169 Video Data Path Chip* and *7191B Digital Multistandard Decoder*
   data sheets); Glossary (CIVIC, Sebastian, video frame buffer).
2. Macintosh Quadra 840AV / Centris 660AV boot ROM (2 MB mask ROM, release $10F3, version word
   $077D, image checksum $5BF10FD1, mapped at $40800000; `CPU address = $40800000 + file offset`)
   — annotated disassembly, resource-container decode and declaration-ROM analysis, plus
   byte-pattern scans of the whole image. Cited sites: the universal declaration ROM at file
   offsets $1D5068–$1FFFFF (extended format block $1FFFD8 with CRC $2D406934, vendorInfo
   "Friday, June 25, 1993" at $1FFF4A, superDirectory $1FFF94); the CIVIC sResource directory $7D
   at $1FFE0A (57 IDs, 39 distinct records) and the board records "Macintosh 3A" $1FFDDA /
   "Macintosh 3B" $1EFD54; the CIVIC video driver ('scod', `.Display_Video_Apple_Civic`) at
   $1E4AD8, DRVR image $1E4ADC–$1E6F9B with entry points $409E4D40/$409E4FAE/$409E621A/$409E61EC,
   the shared sRsrcType $1E6FBE (drHW $25) and sRsrcName $1E6FA4, and the sRsrcDrvrDir $1E6F9C
   (one copy of the driver in the image, by scan); the PrimaryInit sExec at $1FEE70 with its code
   at $1FF1EC, board table $1FFAC0, CivicModeList $1FFBC0 ($1FFBC2–$1FFBF5, 52 mode IDs), and the
   `'Nano'` VRAM-probe constant at $1FF396; sVidParmDir at $1FEA02 with the 52 parameter nodes at
   $1FC0E8–$1FEA01 (the Hi-Res 2 MB node $B1 at $1FD5EC and its 1 MB twin $B0 at $1FD6CE, both
   dumped field-by-field); the sResource geometry records for $B1 (per-depth base offsets and
   rowBytes) and the 'Mac Std Gamma' block $1E49A4; the boot PICTs $1EFD84/$1E6FFA;
   `VideoInfoCyclone` $152AC and `VideoInfoTempest` $152BC (VRAM physical/logical-32/logical-24
   bases, slot alias $09, slot PRAM $46, super-directory $7D, board $25/$36); the ROM-resident
   video digitizer component ('code' at file offset $1A5200, length $730C) — the CIVIC bit-writer
   $57C0 and bit-reader $5810, the TV-mode resynchronization $00BA–$0102, the VDC-interrupt arming
   $5A50 and disarm $5AE0/$5B1A, the field-arrival probe $5BD0, the interrupt service $6EA0 with
   the active-low VDCInt test and the VDCClr 0-then-1 acknowledge, the VInDoubleLine writes $0C8E
   and $28F6; the Sebastian register references $50F30800/$50F30810/$50F30820 at file offsets
   $1AAB0E, $1AAB1A, $1AAB6C, $1A903E, $1AAAFE, $1AAB5E, $1ABDDE, $1ABDF4, $1ABE64; the
   capture-base constant $50200800 at file offsets $1E560E, $1E57DC, $1E63FC, $1E6414, $1A6272,
   $1AB512 and $50200804 at $1AADDC; the pixel-clock synthesizer base $50F2E000 at file offsets
   $1FF6CE and $1E5D7E; and the scans establishing that $50F36000 is never referenced (the ROM
   uses $50036000 exclusively).
3. Apple Computer, Inc., System Enabler 088 (System 7.1 enabler for the Macintosh Quadra 840AV and
   Macintosh Centris 660AV, version 1.0, 1993) — resource inventory and disassembly of the
   enabler's video digitizer build: the digitizer/component pair superseding the ROM's copy; the
   video-liveness probe that stamps $0001FEFF at the capture base + 4 ($50200804), runs one field
   with the capture clock started, and requires the pattern to be overwritten (stride taken from
   CIVIC's VidInSize register); the identical 992-byte stride threshold applied in the digitizer's
   geometry path.
4. Apple Computer, Inc., *Guide to the Macintosh Family Hardware*, second edition, Addison-Wesley
   Publishing Company, 1990 — Chapter 12 "Displays": §"Detecting monitor type" Table 12-2 p. 409
   (the three-sense-line code table, its four "Reserved by Apple" combinations and the "no monitor
   connected" code); §"Video display in the Macintosh SE/30 computer" pp. 409–410 (pseudo-slot
   built-in video as the architectural precedent); §"Video Connector on the Macintosh IIci"
   Tables 12-7 and 12-8 p. 420 (the DB-15 signal set and IIci sense codes). The book predates the
   AV machines and is used here only for the base model CIVIC's sensing extends.
5. Philips Semiconductors, *Desktop Video Data Handbook*, 1994 — the product index and application
   notes for the desktop-video chain Sebastian and the capture chip descend from: SAA7169 "35 MHz
   triple 9-bit D/A converter for high-speed video"; SAA7186 "Digital video scaler"; SAA7192A
   "Digital colour space converter"; and the graphics-environment reference design in which 24-bit
   RGB data feeds the SAA7169 triple DAC for analog RGB output (with the SAA7199B encoder
   downstream), i.e. the palette-DAC-plus-encoder output architecture Sebastian implements for the
   AV machines.
