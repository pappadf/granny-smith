# Platinum — the Catalyst memory and video controller

**Contents:**

1. [Overview](#1-overview) — what the part is, which machine carries it, what it replaces
2. [Register file](#2-register-file) — the $F8000000 window, the bank base registers, the display
   registers, the DACula cells, the frame-buffer aperture
3. [Behaviour](#3-behaviour) — DRAM decode, VRAM organisation and the half bank, raster geometry,
   the retrace interrupt, the hardware cursor
4. [Programming model](#4-programming-model) — POST's memory sizing, the video driver's VRAM
   probe, mode set, sense read and interrupt handler
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 What the part is

**Platinum** (Apple 343S1184) is the memory controller of the Power Macintosh 7200, Apple's
"Catalyst" board. It "controls the memory and cache subsystem, which includes the system bus, the
main memory, the ROM, the VRAM video frame buffer, and the L2 cache" and "supports memory
subsystem sizes up to 1 GB" [1] §"Platinum Memory and Video Controller IC". Unlike the TNT
boards' Hammerhead ([hammerhead.md](hammerhead.md)) it is also the machine's video controller:
there is no Chaos display bus and no Control chip on a 7200, and the on-board frame buffer hangs
off Platinum directly. Its RAMDAC is the **DACula**, whose byte registers sit in Grand Central's
island at +$1B000 — the same cell position the TNT boards give Control's RaDACal
([grand-central.md](grand-central.md) §2.5).

The Open Firmware node is `platinum`, `device_type "display"`, `model "AAPL,343S1184"`,
`AAPL,connector "monitor"`, with a two-entry `reg` — the register window $F8000000/$800 and the
frame-buffer aperture $F1000000/$1000000 — and `AAPL,interrupts` 30 [2]. The `reg` size of the
aperture (16 MB) is the decode, not the fitted VRAM.

The Iridium IC "works with the Platinum memory controller IC to optimize data throughput during
ROM, DRAM, and VRAM data transactions" and accelerates some QuickDraw operations [1] §"Iridium
IC"; nothing in the boot or in the Systems observed (§4) touches an Iridium register.

### 1.2 The machine that carries it

| Machine | Processor | DRAM | VRAM | L2 |
|---|---|---|---|---|
| Power Macintosh 7200/75, 7200/90 | PowerPC 601 at 75/90 MHz, processor bus 37.5/45 MHz | 4 DIMM slots, 8 or 16 MB installed, 256 MB maximum | 1 MB soldered, 2 or 4 MB with VRAM DIMMs | optional 256 KB–1 MB |

[1] Tables 1-1 and 1-2, §"Bus Clock Rates", §"VRAM and VRAM Expansion".

---

## 2. Register file

### 2.1 The register window

128 registers of 32 bits on $10 centres from $F8000000 ($800 bytes, the node's first `reg`
entry), **big-endian** on the processor bus: the drivers use plain `lwz`/`stw` (Linux and
NetBSD's platinumfb use `out_be32`/`in_be32`) [3], [4], unlike the little-endian Grand Central
behind Bandit. Only the first longword of each centre is a register.

| Index | Offset | Name (this page's) | Access | Meaning |
|---|---|---|---|---|
| 0 | +$000 | identifier | R | upper halfword $3001 — the Catalyst identity (§4.3) |
| 2 | +$020 | — | W | POST writes $0E early, the System $0B/$0E around a speed change (§4.1) |
| 4 | +$040 | — | W | POST writes 0 |
| 5 | +$050 | — | W | POST writes $1F4 (500); the System writes $FA (250) and back to $1F4 (§4.1) |
| 6–13 | +$060–+$0D0 | bank base 0–7 | R/W | DRAM bank base addresses (§2.2) |
| 16 | +$100 | scan start | R/W | physical address of the scan start; pixel 0 is 16 bytes past it |
| 18 | +$120 | pitch | R/W | bytes per scan line |
| 19 | +$130 | mode | R/W | written with the depth on a mode set (2 at 16 bpp, 1 MB) |
| 20 | +$140 | VRAM configuration | R/W | $1000 = 64-bit (full-bank) organisation, $0011/$1011 the 1 MB / 2 MB+ settings |
| 21 | +$150 | — | W | POST writes 0; drivers write $100 |
| 22 | +$160 | — | W | drivers write 1 |
| 23 | +$170 | monitor sense | R/W | §2.3 |
| 24 | +$180 | display off | R/W | 0 = raster on; the drivers write 6/7/3/2 around a mode set |
| 27 | +$1B0 | — | W | POST writes $1F4 |
| 32–57 | +$200–+$390 | timing set | R/W | 26 raster registers (§3.3) |
| 34 | +$220 | status | R | bit 2 = in vertical blank (§3.4) |
| 35 | +$230 | retrace acknowledge | R | reading it drops the retrace interrupt (§3.4) |
| 58 | +$3A0 | offset | R/W | per-depth value written with the mode |
| 74 | +$4A0 | — | W | written during the mode set |
| 75 | +$4B0 | — | W | 1/0 around the System's speed change (§4.1) |

Every register not named reads back what was written (*inferred* — no driver or firmware path
observed reads one back).

### 2.2 The bank base registers

"The Platinum IC contains a bank base register for each bank of main RAM. Each bank base
register has space for a base address and control bits. The control bits set the address
multiplexing mode. ... The base address for each bank is based on the sum of the sizes of all the
lower-numbered banks" [1] §"Bank Base Registers". Eight registers at +$060..+$0D0, bank k at
+$060 + $10k — four DIMM slots of up to two banks each; the smallest bank is 4 MB and the
largest 128 MB [1] §"RAM DIMMs".

The register holds the bank's **base address itself**, big-endian, on a 4 MB grain: POST writes
k × $08000000 into bank k to probe and then the running sums of the sizes it found (§4.1), with
the low bits zero in every write observed. Where the address-multiplexing control bits sit is not
established (§6).

### 2.3 Monitor sense (+$170)

Bits 2/1/0 are sense lines A/B/C. Writing a 1 releases a line, writing 0 drives it low. The
read returns the lines **inverted**: a bit reads 1 where its line is low — driven low by the
write, or strapped to ground by the monitor. The drivers walk the usual Apple extended-sense
sequence (all released = 7, then 3, 5, 6 to drive A, B, C in turn); Linux complements the read
before decoding it for the same reason [3]. Observed: with lines A and B floating and C grounded
(the 13" RGB strap) presented un-inverted, the ROM's video driver picks the 640×870 portrait
mode — Apple code 1, the bitwise complement — which is how the inversion was established [2].

### 2.4 DACula (Grand Central +$1B000)

Four byte cells on $10 centres, the RaDACal shape [3], [4], [5]:

| Offset | Cell | Meaning |
|---|---|---|
| +$00 | address | CLUT entry / indexed-register number |
| +$10 | d1 | hardware-cursor palette data: eight entries × R,G,B, auto-advancing from the address |
| +$20 | d2 | indexed register at the address |
| +$30 | CLUT | palette data, R,G,B, the entry auto-advancing after B |

The indexed registers observed:

| Index | Meaning |
|---|---|
| $10/$11 | hardware-cursor X, high/low, biased by one |
| $20 | control: bits 3:2 depth (0/1/2 = 8/16/32 bpp), bit 1 hardware cursor on, bit 4 set with 2 MB or more of VRAM (Linux masks it off for 1 MB) [3], bit 0 written set by Linux, cleared by MkLinux "to clear the cursor" [5] |
| $23 | which pixel-clock pair drives the dot clock: 2 = $24/$25, 3 = $27/$28 [3] |
| $24/$25/$26, $27/$28/$29 | the two pixel-clock parameter sets (the third byte written $C6 by the ROM driver) |
| $40 | reads the DACula type: $84 or $3C, two variants with different clock formulas [3], [4] |

For the $84 part the dot clock is 15 MHz × c0 / ((c1 & $1F) + 2) / 2^(c1 >> 5), with (c0, c1)
the selected pair [3].

### 2.5 The frame-buffer aperture

$F1000000–$F1FFFFFF decodes VRAM linearly; VRAM is 1 MB soldered, expandable to 2 or 4 MB in
three 112-pin VRAM DIMM slots. "All VRAM is addressed as linear memory and appears as a single
memory space. VRAM is organized in one half bank (32-bits wide), one, or two banks of 256K by 64
bits. Each full bank contains 2 MB of VRAM" [1] §"VRAM and VRAM Expansion". Space past the
fitted VRAM does not read back what is written — which is how every driver sizes it (§4.2).

---

## 3. Behaviour

### 3.1 DRAM decode

Each bank decodes its DRAM from its base register's address. A bank smaller than the probe
stride repeats through the window up to the next bank's base (POST's probe reads the highest
address holding its pattern and then looks for the alias — §4.1); an empty bank decodes nothing.
After POST every populated bank sits at the running sum of the lower banks' sizes and every empty
bank at the end of memory, so the populated banks form one contiguous region from 0 — "linear
memory organization only ... no performance gains are seen when two DIMMs of the same size are
installed" [1] §"Data Interleaving", §"RAM DIMMs".

### 3.2 VRAM organisation and the half bank

The soldered 1 MB is a **half bank**: 32 bits wide. With register 20's $1000 bit set (the 64-bit
organisation every driver selects to size VRAM) the half bank backs only the first longword of
each 8-byte doubleword; the second longword does not hold what is written. The ROM's video driver
relies on this to tell 1 MB from 2 MB (§4.2) and then programs the 32-bit organisation (register
20 = $0011) in which the 1 MB is linear. A full 2 MB bank holds both longwords; a second full
bank answers from +$200000.

### 3.3 Raster geometry

The timing set is 26 registers at index 32..57. From the drivers' mode tables [3] and the values
the ROM's driver writes:

| Index | Meaning |
|---|---|
| 44 | horizontal total − 1, in horizontal units |
| 48 / 49 | horizontal display start / end, in horizontal units |
| 51 | vertical total, in half-lines |
| 55 / 56 | vertical display start / end, in half-lines |

Active width = (r49 − r48) × unit, active height = (r56 − r55) / 2. The horizontal unit is one,
two or four dot clocks by mode: the 512-wide modes count single dots, the 640–1152-wide modes
pairs, the 1280-wide modes quads [3]. Every mode's pitch carries a 32-byte margin
(640 × bytes-per-pixel + 32, e.g. 1312 at 16 bpp, 672 at 8 bpp) and pixel 0 is 16 bytes past the
scan start in register 16 [3]; the 16 bytes before each line's pixel 0 hold the hardware-cursor
plane (§3.5). Register 24 nonzero blanks the raster.

The frame rate is dot clock / (htotal × vtotal), htotal = (r44 + 1) × unit and vtotal = r51 / 2.
The ROM's driver programs 640×480 at 16 bpp on the 13" strap with clock pair ($63, $64) — 99 ×
15 MHz / 6 / 8 = 30.94 MHz on the $84 formula — over an 864-dot total, Apple's 640×480 at
≈67 Hz [2].

### 3.4 The retrace interrupt

Grand Central line 30 (the node's `AAPL,interrupts`). Every vertical retrace raises it as a
**level** that stays up until the driver reads register 35 (+$230): the native interrupt routine
the ROM's video driver installs is two loads of $F8000230 followed by its retrace service, and
nothing else in the handler touches Platinum or the DACula [2]. The NanoKernel classifies a
Grand Central interrupt from the live levels under the mask, so a retrace presented as an edge
alone never reaches the driver ([grand-central.md](grand-central.md) §3.4). The driver applies the
gamma and CLUT loads it defers in that service: under System 7.6 a missing retrace interrupt
leaves the startup grey ramp in the CLUT and a drawn desktop displayed as a single grey [2].
Register 34 bit 2 reports vertical blank for polling; the ROM's driver spin-waits on it during a
mode set.

### 3.5 The hardware cursor

The DACula's cursor is the RaDACal scheme (Control's; [pm7500.md](pm7500.md)): enabled by
control bit 1, X from indexed registers $10/$11 (biased by one), Y implicit — the image plane is
the 16 bytes before each scan line's pixel 0, 16 pixels at four bits (bit 3 opaque, bits 2:0 a
cursor-palette entry), and the driver clears the old rows and draws the new. The eight-entry
palette streams through d1 in 24-byte bursts (white, black, then six further entries) [2].

---

## 4. Programming model

### 4.1 POST: memory sizing

Observed, v1 ROM [2]:

1. $FFF043C0–$FFF04418: register 4 = 0, register 5 = $1F4, register 21 = 0, register 27 = $1F4,
   register 2 = $0E.
2. $FFF04540: banks 7 down to 0 are set to k × $08000000 — one 128 MB window per bank — and each
   window is probed.
3. $FFF045C0: banks 0..7 are rewritten with the running sums of the sizes found; empty banks
   take the end of memory (a single 16 MB bank: bank 0 = 0, banks 1–7 = $01000000).

Later, from the NanoKernel/68k side, register 5 = $FA, register 75 = 1, register 2 = $0B then
$0E, register 75 = 0, register 5 = $1F4 — read as a refresh or timing change around a clock
measurement (*inferred*).

### 4.2 The video driver's VRAM probe

The ROM's `.Display_Video_Apple_Platinum` driver [2]: register 20 = $1001 (64-bit organisation),
register 16 = $F1000000, register 24 = 6; it writes "Rush" ($52757368) at +$200000 and "2112"
($32313132) at +$200004, two further patterns at +0 and +4, and reads all four back. All four
intact: two full banks (4 MB). +$200000 lost but +0/+4 intact: one full bank (2 MB). +$200000
lost and one of +0/+4 lost: the half bank (1 MB). Linux probes at +1/+2/+3 MB instead [3].

### 4.3 Identity

The 68k identification routine (ROM $FFC14844) compares the first byte of $F8000000 with $39 (a
TNT Hammerhead) and otherwise takes the Catalyst branch when the swapped longword's low halfword
— the identifier's upper halfword — is $3001; the result is the Catalyst ProductInfo,
**BoxFlag $66, Gestalt 108**, the developer note's machine ID [1] §"Machine Identification", [2].
Open Firmware dispatches on the same byte and builds the `platinum` node on its Catalyst arm
([hammerhead.md](hammerhead.md) §4.3).

### 4.4 Mode set

Display off (register 24 = 7), DACula control, the clock pair into the inactive set and register
$23 flipped to it, the timing set, scan start, pitch, mode and VRAM configuration, the cursor
position and palette, then register 24 = 0. The 1 MB driver offers 8 and 16 bpp at 640×480 —
"With 1 MB VRAM: ... 640 by 480 — 8, 16" [1] Table 2-1 — and Mac OS starts at 16 bpp.

---

## 5. Quirks & errata

- **The sense read is inverted** (§2.3). A model that returns line levels hands every driver the
  complement monitor code.
- **The 1 MB is a half bank** (§3.2). A model that backs the soldered VRAM linearly in the 64-bit
  organisation makes the driver size it as 2 MB and select 32 bpp modes the VRAM cannot hold.
- **The retrace is a level acknowledged by a read** (§3.4). An edge-only retrace boots System
  7.5.3 but leaves System 7.6 and Mac OS 8.1 on a grey screen over a running Finder.
- **Pixel 0 is 16 bytes past the scan start** (§3.3).
- **The DACula is not in the device tree.** Every driver hard-codes $F301B000 [3], [4], [5].

## 6. Open questions

1. The address-multiplexing control bits of the bank base registers, and whether a bank smaller
   than its 128 MB window aliases through it (assumed) or decodes once.
2. The meaning of registers 2, 4, 5, 21, 22, 27, 74, 75 and the per-depth offset (58), and of
   registers 19 and 26.
3. What register 35 returns, and whether register 34 has other status bits.
4. Whether the retrace interrupt has an enable in Platinum, or only the Grand Central mask.
5. The DACula d1 cell beyond the cursor palette, and the full indexed register set.
6. The Iridium accelerator's registers.
7. Whether a $3C DACula (the "old" part, with a different clock formula [3]) ever shipped in a
   7200.

## References

1. Apple Computer, Inc., *Developer Note: Power Macintosh 7200 Computer*, Developer Press, 1995 —
   Table 1-1 (comparison), Table 1-2 (configurations), §"Machine Identification",
   §"Platinum Memory and Video Controller IC", §"Data Interleaving", §"Bank Base Registers",
   §"Iridium IC", §"Bus Clock Rates", §"VRAM and VRAM Expansion" and Table 2-1, §"RAM DIMMs".
2. Power Macintosh 7200/7500/8500/9500 boot ROM, August 1995, 4 MB (`$96CD923D`, `$9630C68B`):
   the Open Firmware `platinum` node; the disassembled 68k identification routine ($FFC14844);
   POST's bank sizing ($FFF04540, $FFF045C0); the `.Display_Video_Apple_Platinum` native driver's
   VRAM probe, mode set, sense walk and interrupt routine, observed running under System 7.5.3,
   System 7.6 and Mac OS 8.1.
3. Linux kernel source, `drivers/video/fbdev/platinumfb.c` and `platinumfb.h` (Platinum
   frame-buffer driver, F. Sirl, J. Howell, after P. Mackerras and M. Abene) — register
   endianness, the 20-mode timing table, the DACula clock formulas and types, the VRAM probe.
4. NetBSD source, `sys/arch/macppc/dev/platinumfb.c` and `platinumfbreg.h` — the Open Firmware
   property dump of a 7200's `platinum` node, the register and colormap geometry.
5. The MkLinux Project (Apple Computer, Inc. / OSF Research Institute), MkLinux DR3 source
   release — `POWERMAC/video_platinum.c` (the DACula address and the cursor-control bit).
