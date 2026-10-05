# Ariel II — the PDM video RAMDAC

**Contents:**

1. [Overview](#1-overview) — what the part is, the four-chip pipeline, which machines carry it,
   clocking, the Sonora software model
2. [Register file](#2-register-file) — the $50F2xxxx decode, the four Ariel II registers, the three
   AMIC video-control registers, the slot-interrupt bank, reset state
3. [Behaviour](#3-behaviour) — the DRAM framebuffer, fetch arbitration, raster timing, pixel
   formats, monitor sensing, VBL interrupts, sync generation
4. [Programming model](#4-programming-model) — slot identity, declaration data, the cold-boot init
   sequence, the framebuffer allocation, the Sonora driver
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 What the part is

The Ariel II is the RAMDAC — color lookup table (CLUT) plus triple video digital-to-analog
converter — that terminates the built-in video path of the first-generation Power Macintosh
platform, Apple codename PDM [1]. The Developer Note describes it plainly: "The Ariel II video
chip provides a color lookup table (CLUT) and digital-to-analog converter (DAC) for driving an
AudioVision monitor" [1] §"Ariel II Video Chip", p. 16. On the 8100 logic board the part is
labelled **"ARIEL-60MHZ"**, U8 [5] sheet 17 (*observed*).

The defining property of the platform it sits in is that **there is no video RAM on the logic
board**. Built-in video scans a framebuffer that lives in ordinary system DRAM [1]
§"Built-in Interface for Video Monitors", p. 35: "support built into the main logic board for
video monitors up to 16 inches in size, using system RAM for frame buffering" [1] p. 35. Four
chips divide the work (*observed* in the 8100 schematics [5]; the Developer Note's prose
matches [1] pp. 15–16):

```
                 601 CPU bus (64-bit)
                        |
   +--------+   VREQ    |       +-------------+ RawPixData<15..0> +----------+  R,G,B
   |  AMIC  |---------->| HMC -->| 2x Data Path|------------------>| Ariel II |-------> HDI-45
   |        | VID_REQ   |       |  chip FIFOs  |                   | CLUT/DAC |   (J6)
   | video  |           |DRAM   +-------------+                   +----------+
   | timing |  FIFO_READ (GfxReadEven*/GfxReadOdd*)                    ^ ^ ^
   |  gen.  |---------------------------------------------------------------+ | |
   |        |  CBLANK-->BLANK, DacClk-->DOTCLK, DacCS*/DacLd*/BufAddr<0..1>| |
   |        |  HSYNC*/VSYNC*/CSYNC* -------------------------------> HDI-45 |-+
   |        |<- MON_ID<0..2> (sense lines, 10k pull-ups) <--- HDI-45 --------+
   +--------+
```

- **[HMC](hmc.md)** (memory side): owns DRAM, arbitrates the CPU bus, and services
  video-refresh requests. On the AMIC's `VID_REQ` (net `GfxRequest*`, HMC pin VREQ) it fetches
  framebuffer data from DRAM **as eight-cycle bursts** [1] §"Data Path Chips", p. 16 and
  writes it into the two data path chips' FIFOs (`GfxWriteOdd*`/`GfxWriteEven*`). The AMIC's
  `FIFO_RESET` (net `GfxReset*`) resets both the HMC's video fetch state (VRST) and the data
  path FIFOs [5] (*observed*).
- **Data path chips (2)**: route byte lanes between 8- and 16-bit I/O and the 64-bit CPU bus,
  and "provide first-in, first-out (FIFO) buffering for video monitor data, which is fetched
  from RAM as eight-cycle bursts" [1] p. 16. Their pixel ports drive `RawPixData<15..0>` — a
  **16-bit pixel bus** — into Ariel II [5] (*observed*).
- **[AMIC](amic.md)** (I/O side): *the video timing generator*. It reads the FIFOs out
  (`GfxReadEven*`/`GfxReadOdd*`), synthesizes the **dot clock** from its four fixed clock inputs
  (57.2832 / 45.1584 / 31.3344 / 25.175 MHz — pins C57M/C45M/C31M/C25M [5]) and feeds it to
  Ariel (`DacClk` → DOTCLK), generates **HSYNC\*/VSYNC\*/CSYNC\*** directly to the monitor
  connector and **CBLANK\*** to Ariel's BLANK pin, latches the vertical-blanking (VBL)
  interrupt, and owns the three **monitor sense lines** (MON_ID0–2) [5] (*observed*).
- **Ariel II**: CLUT + triple DAC only. A 16-bit pixel input port, four byte registers, and
  sync-on-green capability (SYNC pin → `DacCsync*`) [5] (*observed*).

Because video fetch competes with everything else for the memory bus, video refresh holds the
second-highest CPU-bus arbitration priority, below DRAM refresh and above all I/O DMA and the
main processor [1] Table 2-4 p. 20 — and the Developer Note warns that "video refresh actions
may sometimes absorb much of the bus capacity" [1] §"CPU Bus Arbitration", p. 21.

### 1.2 The machines that carry it

Every model of the first Power Macintosh generation carries the same built-in video path;
the AV and high-performance-video (HPV) expansion cards of the AV and 7100/8100 models are
separate channels with their own VRAM frame buffers [1] pp. 5–6, 39–41 and do not use Ariel II.

| Machine | Bus clock | Built-in video | Second channel |
|---|---|---|---|
| Power Macintosh 6100/60, /60AV | 30 MHz [1] Table 2-2 p. 19 | this path | none / AV card |
| Power Macintosh 6100/66 | 33 MHz [2] Table 1-2 | this path | none |
| Power Macintosh 7100/66, /66AV | 33 MHz [1] | this path | HPV 1 MB / AV card |
| Power Macintosh 7100/80 | 40 MHz [2] | this path | HPV card |
| Power Macintosh 8100/80, /80AV | 40 MHz [1] | this path | HPV 2 MB / AV card |
| Power Macintosh 8100/100 | 33.3333 MHz [2] | this path | HPV card |
| Power Macintosh 8100/110 | 36.6667 MHz [2] | this path | HPV card |

The Enhanced models (6100/66, 7100/80, 8100/100, 8100/110) revise the ROM and some ASICs
[2] §"Revised ROM and ASICs" but change nothing in the built-in video path (*inferred*: the
Enhanced note documents no video-path change, and the same driver and register model serve
them). The onboard interface is always the AudioVision HDI-45 socket; the HPV and AV cards add
a DB-15 [1] pp. 39–41.

The 6100/60 is the bandwidth-worst case: its 30 MHz bus serves a 60 MHz CPU and the video
fetches simultaneously, with no VRAM card to offload the raster (*inferred* from
[1] Tables 1-2/2-2 and the arbitration order of Table 2-4).

### 1.3 Clocking

There is **no software-programmable clock synthesizer** in the built-in video path. The AMIC
synthesizes the dot clock by selecting and dividing among four fixed oscillators [5] sheet 11
(*observed*); the ROM source confirms the software contract: "On PDM CPUs, the dot-clock is set
up automatically" — i.e. writing the monitor code to the mode register (§2.4) programs the whole
timing set, dot-clock selection included [3] `SonoraPrimaryInit.a` (comment at :351). The fixed
oscillators [1] Table 2-3 p. 20, [5]:

| Clock | Frequency | Use |
|---|---|---|
| I/O | 31.3344 MHz | I/O components, 12-inch monitor, SCC [1] Table 2-3 |
| VGA | 25.175 MHz | VGA monitor timing [1] Table 3-10 p. 39 (Table 2-3 prints 25.1570; see §5) |
| Dot | 57.2832 MHz | 16-inch monitor (and Portrait) [1] Table 3-10 |
| Sound | 44.1584 MHz [1] Table 2-3 / 45.1584 MHz [5] sheet 11 | sound sample rate — **not** a video clock |

The 12-inch RGB timing uses the 31.3344 MHz "I/O" oscillator halved (15.6672 MHz); the
Portrait and 16-inch timings share the 57.2832 MHz "Dot" oscillator [1] Table 3-10, [3]
`SonoraDeclVideo.r`. Ariel's DOTCLK pin receives the selected clock from the AMIC through
`DacClk` (51 Ω series) [5] (*observed*).

### 1.4 The software model — "Sonora with no VRAM"

Apple did not write a new driver for the PDM machines. The LC III's **Sonora** built-in video
driver (`.Display_Video_Apple_Sonora`, driver ID `drHwSonora`, version 0.1 on PDM versus 0.0 on
the LC III) was extended instead: the AMIC implements the Sonora video-register model — mode,
depth and sense bytes plus the slot-interrupt bytes — and Ariel II implements the V8/Ariel
CLUT/DAC register model [3] `DepVideoEqu.a` :398/:419, `SonoraDriver.a`. The driver name string
`.Display_Video_Apple_Sonora` is present in the shipping ROM at offset $2F17ED [4]. Every
software-visible register in this page is named with its Sonora equate.

## 2. Register file

### 2.1 Address decode

The whole $50xxxxxx block is the I/O segment of the fixed physical map (RAM $00000000–,
ROM $40800000 and aliases, I/O $50000000–$5FFFFFFF) [1] Table 2-5 p. 22. The video-relevant
decodes [3] `UniversalTables.a` ("HMCDecoderTable"), `HardwarePrivateEqu.a` :1260–1305:

| Address | Contents | Notes |
|---|---|---|
| $50F00000 | pseudo-VIA1 base (AMIC) | IFR at $50F01A00, IER at $50F01C00 — the classic VIA register numbers ($D/$E) at the standard $200 stride [1] Figure 2-2 p. 23 |
| **$50F24000** | **Ariel II CLUT/DAC** (`VDACAddr`) | 4 byte registers (§2.2) |
| $50F26000 | pseudo-VIA2 / "RBV" block (AMIC) | slot and device interrupt registers (§2.5) |
| **$50F28000** | **video control** (`SonoraVdCtlBase`) | 3 byte registers (§2.4) |
| $50F30000 | AMIC base (`AMICBase`) | the DMA channels at +$1000 etc. — not video [3] |
| $50F40000 | [HMC](hmc.md) base | serial configuration register; not touched by video code [3] |
| $60B00000 | **logical** framebuffer address (`VRAMLogAddr32`) | an MMU mapping onto DRAM (§3.2) — *not* in the I/O segment |

Ariel's register port has exactly two register-select lines, RS0/RS1 ← `BufAddr<0..1>`, with
chip select ← `DacCS*`, read/write ← `DacLd*`, and data D0–7 ← `IOData<0..7>` — hence exactly
four byte registers [5] sheet 17 (*observed*). Accesses to mapped-but-undecoded addresses make
the AMIC assert an error signal after 40 µs, a condition not generally recoverable [1]
§"Address Errors", p. 21.

### 2.2 Ariel II registers

| Off | Name | R/W | Function |
|---|---|---|---|
| 0 | `ArielAddrReg` | r/w | CLUT address. Writing sets the CLUT index for subsequent data accesses. |
| 1 | `ArielDataReg` | r/w | CLUT data. Three successive accesses are R, G, B (8 bits each); after the blue byte the address auto-increments. |
| 2 | `ArielConfigReg` | r/w | Control. Low 3 bits = depth code (same encoding as §2.4); upper bits = mode/overlay control. |
| 3 | `ArielKeyReg` | r/w | Key color register (overlay / chroma key). Unused by the PDM driver. |

Ariel II is register-compatible with the LC's V8/Ariel DAC — the driver drives both with the
same `V8DACxxx` offsets 0–3 [3] `HardwarePrivateEqu.a` :1092–1103 ("V8/Ariel registers"),
`DepVideoEqu.a` :419.

**`ArielAddrReg` (+0).** An 8-bit CLUT index. The address register is written once to
start a sequential CLUT load and re-written per entry for non-sequential ("index mode")
accesses (§3.4). The shipping ROM's driver also clears it against the Ariel data pointer
(`CLR.B -1(An)` — i.e. a write through data-pointer−1, the adjacent address register) at ROM
offsets $2F1B0A and $2F2608 [4] (*observed*).

**`ArielDataReg` (+1).** The CLUT data port. A load (or read) sequence cycles the register
through three byte phases — red, green, blue — and advances the CLUT address after each
complete triplet, so sequential ranges need only one address write [3] `SonoraDriver.a`
(`SonoraSetVidMode` :657–685, `SonoraSetEntries`). Reads mirror writes through the same two
registers [3]. Whether an address write between data bytes resets the RGB byte phase is not
established (§6).

**`ArielConfigReg` (+2).** The low 3 bits carry the pixel-depth code in the same encoding as
the AMIC depth register (§2.4); the driver's depth change does a read-modify-write that clears
the depth field with the mask `$F8` and ORs in the new code — "clear V8 **and Ariel** control
register screen depth bits (top 5 bits)" [3] `SonoraDriver.a` :2342–2364, `DepVideoEqu.a`
(the mask comment describes clearing the depth bits *leaving* the top 5 bits; see §5). The
init code writes **$08** — 1 bpp, "master mode", no overlay [3] `SonoraPrimaryInit.a` :938–1000
(the $08 write is at ROM offset $2F3200 [4]). The meaning of the upper bits is otherwise
undocumented (§6).

**`ArielKeyReg` (+3).** The key/overlay color register. The PDM driver never programs it
[3]; its bit semantics are unexercised on this platform (§6).

### 2.3 CLUT access protocol

From the driver's use [3] `SonoraDriver.a`, `SonoraPrimaryInit.a` :950–962:

- **Sequential loads** (SetEntries, gray-CLUT loads): write the index to register 0 once,
  then stream `R,G,B` byte triplets to register 1; the address auto-increments after each
  blue write.
- **Non-sequential writes** reload register 0 per entry.
- Loads are performed at raised interrupt level after a VBL wait (§3.6), so CLUT changes hit
  during blanking.
- **1 bpp convention**: entries $7F (white) and $FF (black) are written at init; at depths
  below 8 bpp the hardware reads the CLUT through a windowed palette (§3.4).
- **Gamma** is applied in software when the driver writes the CLUT — the standard slot-video
  `SetGamma`/`GetGamma` machinery, tables from the shared gamma resource ("Mac Std Gamma",
  "Mac RGB Gamma", "Mac Gray Gamma", "Page-White Gamma", "NTSC/PAL Gamma", all present in the
  shipping ROM at $2F5FB4–$2F7208 [4]). **There is no hardware gamma** [3].
- **16 bpp**: the CLUT is bypassed for color generation, but the driver still loads a linear
  ramp through the same port via `DirectSetEntries` [3].

**Sync-on-green.** Ariel's SYNC pin carries composite sync onto the green output
(`DacCsync*`) [5] (*observed*). No software control bit for it has been identified in the
PDM sources (§6).

### 2.4 The AMIC video-control registers ($50F28000)

Three byte-wide registers, at offsets from `SonoraVdCtlBase` [3]
`HardwarePrivateEqu.a` :1273–1277:

| Off | Name | R/W | Function |
|---|---|---|---|
| $00 | `SonoraVdModeReg` | r/w | bit 7 = **video blank** (1 = blanked, syncs stopped); low bits = **monitor code** (timing select) |
| $01 | `SonoraVdColrReg` | r/w | framebuffer **pixel-depth** code |
| $02 | `SonoraVdSenseRg` | r/w | monitor sense: upper nybble = read-back of the sense lines, lower nybble = drive (§3.5) |

**`SonoraVdModeReg` (+$00).** The low bits select one of the AMIC's canned timing sets by
monitor code; bit 7 (`SonoraVidBlnkBit`) blanks video — sync generation stops, the monitor
shows black. Writing the monitor code programs AMIC's whole timing set: sync widths, porches,
and the dot-clock selection/division (§1.3). The register is written with blank set during any
mode change and with blank cleared to start the raster [3] `SonoraPrimaryInit.a` :938–1000.
At boot, video is shut down by writing **$9F** — blank plus code $1F ("none") [3]
`SizeMem.a` :1256 (but see §6 on the shipping-ROM literal). Monitor codes from the
declaration-ROM per-mode parameter blocks ("Monitor code value") [3] `SonoraDeclVideo.r`
(nodes 2845–2875), confirmed in the shipping ROM's vidParams blocks at $2F2750–$2F28C0 [4]:

| Code | Timing | Dot clock (MHz) |
|---|---|---|
| 1 | Portrait 640×870, 75 Hz | 57.2832 |
| 2 | Rubik 12" 512×384, 60.15 Hz | 15.6672 (31.3344/2) |
| 6 | Hi-Res 13"/14" 640×480, 66.67 Hz | 31.3344 |
| 9 | GoldFish 16" 832×624, 74.55 Hz | 57.2832 |
| 10 | Rubik-560 560×384 | Vail (LC III) only — not reachable on PDM |
| 11 | VGA 640×480, 59.94 Hz | 25.175 |
| 13 | Hi-Res-400 640×400 | Vail only |
| $1F | "none" — no timing set selected | — |

*Bit-width caveat*: the values seen in the wild are $01, $02, $06, $09, $0B, $1F plus bit 7,
so the code field is at least 5 bits (bits 4–0); bits 6–5 are unobserved (§6) [3].

**`SonoraVdColrReg` (+$01).** The driver writes the zero-based mode index straight into this
register [3] `SonoraDriver.a` (`SonoraSetDepth`, :2342–2364):

| Value | Depth | Mode ID |
|---|---|---|
| 0 | 1 bpp | `FirstVidMode` = $80 |
| 1 | 2 bpp | $81 |
| 2 | 4 bpp | $82 |
| 3 | 8 bpp | `FourthVidMode` = $83 |
| 4 | 16 bpp (1:5:5:5) | `FifthVidMode` = $84 |

The same value is OR'd into the low bits of Ariel's config register — the framebuffer
controller and the DAC are always programmed in tandem, during vertical blanking [3].

**`SonoraVdSenseRg` (+$02).** See §3.5 for the register's semantics and driving sequences.

### 2.5 The slot-interrupt bank ($50F26000)

The VBL surfaces through the pseudo-VIA2 "slot" bank that the AMIC implements; the four
byte registers, documented by the Developer Note's interrupt figure and named by the ROM
source [1] Figure 2-2 p. 23 [3] `HardwarePrivateEqu.a` :1263–1298:

| Address | Name | Bits (7→0) |
|---|---|---|
| $50F26002 | `SonoraSlotIFR` — slot interrupt flags | `0, VBL, SLT3, SLT2, SLT1, SLT0, 1, 1` |
| $50F26012 | `SonoraSlotIER` — slot interrupt enables | `SET/CLR, VBL ENA, SLT3 ENA, SLT2 ENA, SLT1 ENA, SLT0 ENA, 0, 0` |
| $50F26003 | `SonoraVIA2IFR` — device bank flags | `VIA2 IRQ, SCSI-B, FDC, 0, SCSI-A, SCSI-B DRQ, ANY SLOT, SCSI-A DRQ` |
| $50F26013 | `SonoraVIA2IER` — device bank enables | same layout, with bit 7 = SET/CLR |

SLT3 and the SCSI-B bits exist only on the 7100/8100 (labelled in *italics* in the Developer
Note's figure, which covers the 7100/66 and 8100/80) [1] p. 23. The flag bits are active-low
and follow classic VIA semantics: a bit reads 0 when pending, and writing 1 to a bit clears
(deasserts) it; the enable registers use the VIA IER SET/CLR form — bit 7 = 1 sets the
enabled bits among the 1-bits written, bit 7 = 0 clears them [3] (*observed* in the driver
idioms, §3.6). Bits 6–2 of the slot IFR OR together into the ANY-SLOT flag (bit 1) of
the device bank, whose bit 7 summarizes into the VIA2 interrupt — 68k interrupt level 2, exactly
like classic slot VBLs [1] Figure 2-2, [3].

### 2.6 Reset state

The power-on values of all seven video-relevant registers (four Ariel, three control) are
unknown. What the boot software establishes (*observed* [3] [4]):

- PrimaryInit's first video action is to disable the VBL (write $40 to `SonoraSlotIER`) —
  it does not rely on any reset value of the enable register.
- The init sequence then un-blanks explicitly before sensing (§3.5) and re-blanks, again
  explicitly; no reset value of the mode register is assumed.
- The depth register and Ariel's config register are both written before the raster starts.
- The sense register is explicitly tristated ($07) before its first read.

## 3. Behaviour

### 3.1 The fetch pipeline and bus arbitration

The frame store being ordinary DRAM, scan-out runs through the memory system. The AMIC's
video timing engine raises `VID_REQ` (net `GfxRequest*`) when its FIFOs need refilling; the
HMC services it at the "video refresh" arbitration priority — second only to DRAM refresh,
above all I/O DMA and the main processor [1] Table 2-4 p. 20 — fetching framebuffer data from
DRAM as eight-beat bursts and writing it into the two data path chips' FIFOs
(`GfxWriteOdd*`/`GfxWriteEven*`) [1] p. 16, [5] (*observed*). The AMIC clocks pixels out of
the FIFOs (`GfxReadEven*`/`GfxReadOdd*`) onto `RawPixData<15..0>`, through 51 Ω + 33 pF per
line into Ariel's PIX0–15 inputs [5] (*observed*). A single `FIFO_RESET` from the AMIC
resets the HMC's fetch state and the data path FIFOs together [5] (*observed*).

The cost of this design is DRAM bandwidth: every active line of the raster is a set of
eight-cycle bursts contested by the CPU, DMA and refresh. The Developer Note's PDS design
rules state it for expansion cards, and it applies equally to the processor: "video refresh
actions may sometimes absorb much of the bus capacity" [1] p. 21.

### 3.2 The framebuffer — three addresses, no window

There is **no fixed framebuffer hardware window**. The frame store is ordinary DRAM,
allocated at boot, and there are three separate notions of its address [3]:

1. **Physical — forced to address 0.** The boot-time allocator (`HMCMerge` in the Start
   Manager's memory sizer) allocates the framebuffer with alignment mask −1, i.e. **forced
   to physical address $00000000** — the start of the soldered 8 MB DRAM bank — with the
   source comment "(HMC Requirement)" [3] `SizeMem.a`. Size allocated: **604 KB** (618,496
   bytes; "max RBV can support w/slop" — the largest real mode is 640×480×16 = 614,400 bytes,
   and QuickDraw is known to write past the end of a frame, hence the slop). The pages are
   made **write-through cacheable** [3].
2. **Logical — $60B00000.** The nanokernel maps that allocation at `VRAMLogAddr32` =
   **$60B00000**; the `VideoInfoPDM` record gives physical base $00000000, logical base
   $60B00000, no 24-bit base [3] `UniversalTables.a`. Verified in the shipping ROM at offset
   $15FD6: physical 0, logical $60B00000, PRAM slot $09, directory $7E, board sResource $3B,
   driver ID `drHwSonora` = $0022 [4] (*observed*). The address is not arbitrary: $60B00000 is
   the LC III (Vail) VRAM base — Vail's own video record (ROM offset $15F98) reads
   physical=logical32=$60B00000, logical24=$00B00000, PRAM slot $0B — so the shared Sonora
   driver sees the framebuffer at the same logical address on both machines [4] (*observed*).
3. **Scan-out — the low bank, hard-wired.** The HMC/AMIC fetch engine reads physical DRAM
   from the low bank; **no software-visible scan-base register exists** — the ROM never
   writes a framebuffer base pointer anywhere [3] (*observed*). The only platform knob is
   [HMC](hmc.md) serial-configuration bit 33: set, fetch from physical $00000000 (the ROM's
   constant state); clear, from $00100000 (*inferred — unverified*; the released MkLinux
   kernel for these machines runs with the bit clear while placing its frame buffer at
   physical $00100000 [7]) — see [hmc.md](hmc.md) §2.3. The forced-to-zero allocation is
   exactly why: the hardware fetch base is not programmable through the video registers.

The framebuffer is allocated **only when a monitor is present**: the allocator senses the
monitor (including the extended-sense walk, §3.5) before the OS is up; if nothing is
connected and no factory "burn-in" signature (`'RNIN'`, `'SRNN'`, `'RN12'`, `'RN13'`,
`'RN15'`, `'RN16'` in parameter RAM $FC–$FF) is found, **no video RAM is allocated at all**
and the 604 KB stays with the system [3] `SizeMem.a` `HMCMerge`.

Also allocated at the same boot stage (context for the memory map, not video): a 160 KB
AMIC DMA buffer, logical $61000000, physically 256 KB-aligned ("AMIC Requirement"),
non-cacheable [3].

### 3.3 Raster timing

The AMIC's timing generator implements one canned timing set per monitor code; the Developer
Note publishes them as Table 3-10 ("Apple monitor timing values") [1] p. 39, whose columns
map to the codes of §2.4 (12" RGB = code 2, 13" RGB = code 6, Portrait = code 1, 16" RGB =
code 9, VGA = code 11 [3]):

| Parameter | 12" RGB | 13" RGB | Portrait | 16" RGB | VGA |
|---|---|---|---|---|---|
| Image size, pixels | 512×384 | 640×480 | 640×870 | 832×624 | 640×480 |
| Dot clock, MHz | 15.6672 | 31.3344 | 57.2832 | 57.2832 | 25.175 |
| Line rate, kHz | 24.480 | 34.975 | 68.850 | 49.725 | 31.469 |
| Frame rate, Hz | 60.15 | 66.62 | 75.00 | 74.55 | 59.94 |
| H sync space, dots | 128 | 256 | 192 | 320 | 160 |
| H image space, dots | 512 | 640 | 640 | 832 | 640 |
| H line length, dots | 640 | 896 | 832 | 1152 | 800 |
| H front porch, dots | 16 | 80 | 32 | 32 | 16 |
| H sync pulse, dots | 32 | 64 | 80 | 64 | 96 |
| H back porch, dots | 80 | 112 | 80 | 224 | 48 |
| V sync space, lines | 23 | 45 | 48 | 43 | 45 |
| V image space, lines | 384 | 480 | 870 | 624 | 480 |
| V screen length, lines | 407 | 525 | 918 | 667 | 525 |
| V front porch, lines | 1 | 3 | 3 | 1 | 10 |
| V sync pulse, lines | 3 | 3 | 3 | 3 | 2 |
| V back porch, lines | 19 | 39 | 42 | 39 | 33 |

(All values as printed in [1] Table 3-10 p. 39; "sync space" = front porch + sync + back
porch.) The monitors the built-in path supports, with their documented maximum depths and
frame sizes, are [1] Table 3-8 p. 36:

| Monitor | Pixels | Color depth (bits) | Frame size (bytes) |
|---|---|---|---|
| 12-inch RGB | 512×384 | 16 | 393,216 |
| 14-inch RGB | 640×480 | 16 | 614,400 |
| 15-inch portrait | 640×870 | 8 | 556,800 |
| 16-inch RGB | 832×624 | 8 | 519,168 |
| VGA | 640×480 | 8 | 307,200 |

The ROM's own declaration data caps VGA at 16 bpp on PDM, where the Developer Note's table
says 8 — a discrepancy taken to §5. Chapter 1 summarizes the same capability as "monitors up
to 13 inches in size at color depths up to 16 bits and monitors up to 16 inches in size at
color depths up to 8 bits" [1] pp. 5–6.

### 3.4 Pixel formats and the depth-windowed palette

Pixel storage is standard Macintosh big-endian packing: 1/2/4/8 bpp indexed with the
most-significant pixel leftmost; 16 bpp is 1:5:5:5 xRGB, big-endian [3] `DepVideoEqu.a`
(rowbytes equates :1571–1612). Rowbytes are exactly `width × depth / 8` — packed, no
padding [3] `SonoraDeclVideo.r` "Depth Params".

At depths below 8 bpp the hardware does not feed the pixel value straight to the CLUT: it
reads the palette through a **windowed view** of the 256-entry CLUT, the V8/Sonora
convention — a pixel value *i* reads CLUT entry `start + i × skip` with
`skip = 256 / 2^depth` and `start = skip − 1` [3] `SonoraPrimaryInit.a` :950–962
(*observed* in the init code, which writes $7F=white and $FF=black at 1 bpp — exactly the
two window entries a 1-bpp display uses). At 8 bpp the window is the identity; at 16 bpp the
CLUT is bypassed for color generation (§2.3).

### 3.5 Monitor sensing (HDI-45)

The HDI-45 connector carries three **sense lines** — pins 8, 9 and 18, "Monitor ID sense
line 1/2/3" — plus a static "Monitor ID" ground key on pins 31/32/41 [1] Table 3-9 p. 37:

| Pin | Description | Pin | Description |
|---|---|---|---|
| 3, 4 | Left / right channel audio input | 27 | Red video output (75 Ω) |
| 5, 6 | Left / right channel audio output | 28 | I²C data signal |
| 8 | Monitor ID sense line 1 | 29 | I²C clock signal |
| 9 | Monitor ID sense line 2 | 31, 32, 41 | Monitor ID |
| 10, 11 | Green ground / Green video output (75 Ω) | 33 | Vertical sync signal |
| 13 | Power for camera +5 V | 34 | Composite sync signal |
| 18 | Monitor ID sense line 3 | 35–37 | ADB power / ground / data |
| 20, 21 | S-video input luminance (Y) / chroma (C) | 42 | Horizontal sync signal |
| 23–25 | Reserved | 44, 45 | Blue ground / Blue video output (75 Ω) |

(Abridged from [1] Table 3-9 p. 37; audio, camera and S-video input pins omitted from this
extract.) The sense lines reach AMIC pins MON_ID0–2 with 10 kΩ pull-ups [5] (*observed*).
The I²C pair (pins 28/29) goes to the Cuda microcontroller, not the AMIC — it is the
AudioVision monitor-control channel, not a sensing path [5], [1] Table 3-9. For the
monitor-side strapping conventions, Apple Technical Note 326 is the reference [1] p. 36,
[6].

**Register model** (`SonoraVdSenseRg`, $50F28002) [3] `DepVideoEqu.a` :1220–1237: the
**upper nybble returns the three sense lines, the lower nybble drives them**. Within each
nybble, **bit 2 = line A, bit 1 = line B, bit 0 = line C** (`sonoraSenseLineA/B/C`). Writing
a 0 in a bit position pulls that line low; writing 1 releases it. The lines float high
through the pull-ups, and a monitor's strapping ties a subset to ground; the read-back
therefore reflects the wired state of host-drive AND monitor strap — an open-collector
wired-AND (*inferred — unverified*: consistent with the drive values, the pull-ups and the
extended-sense sequences, but not stated as such in any source). A settle delay
(`SonoraDelay`, ≈ 500 µs of VIA accesses) precedes each read [3].

Drive values used by the production software [3] `SonoraPrimaryInit.a`:

| Write | Meaning |
|---|---|
| $07 (`%111`) | tristate / release all lines |
| $03 (`%011`) | drive line A low |
| $05 (`%101`) | drive line B low |
| $06 (`%110`) | drive line C low |
| $00 | "EVT1 / broken-AMIC" tristate fallback (bit sense inverted on EVT1 boards, boxflag $44 — not shipped) |

**Simple sense**: tristate ($07), delay, read; the 3-bit result is the classic Apple index
sense code [3]. **Extended sense** (performed when the simple read returns 6 or 7): drive A
and read B,C; drive B and read A,C; drive C and read A,B; assemble the 6-bit code
`BC AC AB`. The codes recognized on PDM: `extendedSenseVGA` → VGA, `extendedSenseGF` →
GoldFish 16" (from the type-7 walk), and `extendedMSB1/2/3` → the Multiple-Scan bands (from
the type-6 walk) [3] `SonoraPrimaryInit.a` `DoSonoraExtendedSense` :257–345. After sensing,
the lines are re-tristated and blanking is re-asserted [3].

**The dot-clock quirk.** PrimaryInit clears the blank bit — starting syncs — *before*
reading the lines, then re-blanks: "Because of a problem in Sonora, the dot clock must be
going in order to sense" [3] `SonoraPrimaryInit.a` :655–663 (*observed* in the source). That
is, the sense lines do not read back correctly with the raster stopped.

**Index code → configuration.** The sensed code selects a row of the PDM configuration
table (`PDMConfigTable`) [3] `SonoraPrimaryInit.a` :1195–1258 (its Rubik entry — mini-gamma
$05 $FF ×3, sRsrc $86 = `GSb`, default mode $83 = 8 bpp — is present in the shipping ROM at
offset $2F3410 [4]):

| Sense | Monitor | PDM default sRsrc / depth |
|---|---|---|
| 0 | (Vesuvio) | not supported |
| 1 | Portrait mono | `FPc`, 8 bpp |
| 2 | Rubik 12" RGB | `GSb`, 8 bpp (16 available) |
| 3, 4 | 2-page mono, NTSC | not supported |
| 5 | RGB Portrait | `RGBFPc`, 8 bpp |
| 6 | Hi-Res 13"/14" | `HRc`, 8 bpp (16 available) |
| 7 | no connect | extended walk; else no video |
| 8 (translated) | VGA | `VGAc`, 8 bpp (16 available) |
| 9 | PAL | not supported |
| $A | GoldFish 16" | `GFb`, 8 bpp |
| $B | 19" | not supported |
| $C/$D/$E | Multiple-Scan 1/2/3 | `MSB1`/`MSB2` families |

Multiple-Scan band 1 shares the Hi-Res timing family; bands 2/3 share the GoldFish family
[3].

### 3.6 Interrupts — the VBL

The VBL is generated by the AMIC's timing core — there is no external VBL line into the AMIC
[5] (*observed*) — and surfaces as **bit 6 of the slot interrupt flag register**
(`SonoraSlotIFR`, $50F26002), the register bank the Developer Note documents as the
pseudo-VIA2 slot bank [1] Figure 2-2 p. 23. To the Slot Manager the built-in video is
**slot $0**, and its slot interrupt handler is installed through the standard
`SIntInstall` path [3] `SonoraDriver.a` :1163–1236.

The driver's four access idioms, all byte-for-byte present in the shipping ROM [3] [4]
(*observed* at the cited offsets):

- **Enable** (driver open / after handler install): write $C0 to `SonoraSlotIER` — SET form
  plus the VBL enable bit. ROM $2F1E64 [3] :1230.
- **Disable** (driver close): wait for a VBL, then write $40 — CLR form, bit 7 = 0 clears
  the VBL enable. ROM $2F1E2A [3] :1204–1208.
- **Acknowledge / clear**: write $40 to `SonoraSlotIFR` (write-1-to-clear; the flag then
  returns to its inactive state, reading 1). ROM $2F240A, $2F2640 [3].
- **Poll** (`SonoraWaitVSync`, used before every CLUT/depth/mode touch): raise the interrupt
  level, write $40 to clear the flag, then spin until **bit 6 reads 0** — i.e. until the next
  VBL asserts the cleared flag. ROM $2F240A/$2F2640 [3] :2299–2325.

The active-low flag polarity is fixed by that poll loop: the spin tests bit 6 and exits when
it reads 0, so a pending VBL drives the bit low [3] (*observed* in the driver code; it also
matches classic VIA active-low flag semantics).

Timing: the VBL rate equals the frame rate of the selected monitor code (§3.3) and is
asserted at the start of vertical blanking [3] (*inferred from the register model and the
WaitVSync usage; no source states the phase explicitly*). The slot bank's assertion also
drops the ANY-SLOT flag (bit 1) of $50F26003, whose bit 7 summarizes into the VIA2 interrupt
at 68k level 2, exactly like classic slot VBLs [1] Figure 2-2, [3].

Do not conflate the VBL with the pseudo-VIA1 **60.15 Hz tick** — IFR $50F01A00 / IER
$50F01C00, bit 1 — a separate, always-60.15 Hz AMIC timer that is not the video raster [1]
Figure 2-2, [3]. (The number is a coincidence: 60.15 Hz is also the 12" RGB frame rate of
Table 3-10 [1].)

History: early PDM bring-up used a Time-Manager pseudo-VBL (`hasSoftVBL`); it was removed
from the tables in October 1993, and shipping machines use the real AMIC VBL [3]
`UniversalTables.a` change log `<MC4>`.

### 3.7 Sync generation and blanking

The AMIC generates **HSYNC\*, VSYNC\* and CSYNC\*** directly and routes them to the HDI-45
connector (pins 42, 33 and 34 respectively), and generates **CBLANK\*** onto Ariel's BLANK
pin; Ariel's SYNC pin carries composite sync onto green (`DacCsync*`) [1] Table 3-9 p. 37,
[5] (*observed*). The blank bit of the mode register (§2.4) is the single software switch:
set, sync generation stops and the monitor shows black; clear, the raster runs. The
Display-Manager-era `SetSync` control call uses exactly this bit to implement DPMS-style
monitor power-down — "Dark Star" in the driver's change log [3] `SonoraDriver.a` (SM20) —
on PDM by stopping syncs only (an Ardbeg-class Sonara variant also had a power register at
+$00A; PDM has only the blank bit) [3].

The Developer Note credits the data path chips with supplying "the Ariel II video chip with
appropriate timing signals for video monitor data" [1] p. 16; the 8100 schematic shows the
actual sources of Ariel's DOTCLK and BLANK strobes as AMIC pins (`DacClk`, CBLANK), with
the data path chips clocking the pixel bus out of their FIFOs under AMIC's `GfxRead*`
strobe [5] (*observed*). The Dev Note's phrasing is a functional summary; the pin-level
ownership is the schematic's.

## 4. Programming model

### 4.1 Slot identity and declaration data

Built-in video is **slot $0** to the Slot Manager ("Built-in video is always Slot $0" [3]
`SonoraPrimaryInit.a`), with its slot parameter RAM stored in the **slot-9 record** — the
classic RBV-style arrangement [3] `UniversalTables.a` `VideoInfoPDM` ("Slot number to use
for PRAM storage = $09"). The declaration data lives in the system ROM's slot-0 blob (ROM
revision 5 directory) under directory `sRsrcSonoraDir` = $7E, board sResource
`sRsrc_BdPDM` = $3B, driver `drHwSonora` = $0022 [3] `DepVideoEqu.a` :2231, :2580–2637 —
all six fields verified in the shipping ROM's `VideoInfoPDM` record at offset $15FD6 [4].

The video functional sResources in the Sonora directory (sResource IDs; per-sRsrc depth-mode
IDs are the standard `FirstVidMode`=$80 … `FifthVidMode`=$84 = 1/2/4/8/16 bpp) [3]
`SonoraDeclVideo.r` :185–260:

| sRsrc IDs | Name | Modes |
|---|---|---|
| $80–$83 | `FP`, `FPa`, `FPb`, `FPc` | Portrait 1 / 1,2 / 1,2,4 / 1,2,4,8 bpp |
| $84–$87 | `GS`, `GSa`, `GSb`, `GSM` | Rubik 1 / …8 / …16 / =`GSb` |
| $88–$89 | `GS560a/b` | Vail only |
| $8A–$8D | `RGBFP`(a,b,c) | RGB Portrait |
| $8E–$92 | `HR`, `HRa`, `HRb`, `HRc`, `MSB1` | Hi-Res; `MSB1` ≡ `HRc` |
| $93–$94 | `HR400a/b` | Vail only |
| $95–$98 | `VGA`(a,b,c) | VGA |
| $99–$9C | `GF`, `GFa`, `GFb`, `MSB2` | GoldFish; `MSB2` ≡ `GFb` |

PrimaryInit prunes this directory down to the single sensed configuration (plus its family,
for multiple-scan monitors), stamps slot PRAM (`SP_Flags` gets `spNoVRAM` set on PDM), and
leaves the chosen sRsrc enabled [3] `SonoraPrimaryInit.a` :700–840. The per-mode parameter
blocks — monitor code, Omega clock triples (unused on PDM), max-mode depth caps, row and
rowbyte counts — are present in the shipping ROM at $2F2750–$2F28C0 in the order GoldFish,
VGA, HR400, Hi-Res, GS560, Rubik, Full-Page, with monitor codes 9, 11, 13, 6, 10, 2, 1
respectively; the PDM "no-VRAM" max-mode triples cap Hi-Res, VGA and Rubik at 16 bpp ($84)
and GoldFish/Portrait at 8 bpp ($83) [4] (*observed*).

### 4.2 Cold-boot initialization sequence

PrimaryInit's hardware sequence at cold start [3] `SonoraPrimaryInit.a` :596–1000
(*observed* in the source; each step's register writes are also in the shipping ROM [4]):

1. Disable the VBL: write $40 to `SonoraSlotIER` (CLR form).
2. VRAM sizing — skipped on PDM (`hasVRAM` false; there is no VRAM).
3. Unblank (`BCLR #7,VdModeReg`) so the dot clock runs — required for sensing (§3.5);
   tristate the sense lines ($07, with a $00 fallback if the read returns 0, the
   broken-AMIC probe); read sense; run the extended walks as needed; re-blank and select
   the configuration from the sensed code (§3.5).
4. Write depth 0 (1 bpp) to `SonoraVdColrReg`.
5. Write the monitor code with the blank bit set to `SonoraVdModeReg`.
6. Write $08 to `ArielConfigReg` (1 bpp, master mode, no overlay); load CLUT entries $7F
   (white) and $FF (black) through Ariel registers 0/1.
7. Paint the 1-bpp gray dither into the framebuffer at $60B00000 — rows × rowlongs from the
   vidParams block, alternating $AAAAAAAA and its inverse per row.
8. Clear the blank bit — the raster starts.

### 4.3 The framebuffer allocation

The 604 KB allocation (§3.2) is made by `HMCMerge` before the OS is up, sized to the largest
mode any built-in configuration can reach plus slop, and covers **every** reachable mode —
so a later on-the-fly resolution switch within the same family does not resize it; only a
restart re-runs the allocation [3] `SonoraDriver.a`, `SizeMem.a`. The PowerPC/nanokernel side
does nothing device-specific: the 68k emulator runs the whole driver, and the nanokernel
only provides the memory mapping that places the allocation at logical $60B00000,
write-through [3] `StartInit.a`, `SizeMem.a`.

### 4.4 The Sonora driver

`.Display_Video_Apple_Sonora` is a dCtl-based slot driver. Control calls implemented [3]
`SonoraDriver.a` :552–572:

| csCode | Call | Notes |
|---|---|---|
| 0 | Reset | |
| 1 | KillIO | |
| 2 | SetVidMode | depth change, VBL-gated |
| 3 | SetEntries | indexed CLUT load, gamma-corrected |
| 4 | SetGamma | software gamma table select |
| 5 | GrayPage | repaint the dither pattern for the current depth |
| 6 | SetGray | |
| 7 | SetInterrupt | install/remove the slot VBL handler |
| 8 | DirectSetEntries | 16-bpp linear-ramp load |
| 9 | SetDefaultMode | |
| $A | Display Manager resolution switch | new-in-PDM path |
| $B | SetSync | DPMS-style power-down ("Dark Star") |

Status calls return mode, entries, pages and base plus Display-Manager connection and
mode-timing information — `GetConnection` reports the multiple-scan family codes;
`GetModeTiming` flags "safe" modes needing no user confirmation (13"/16" safe; VGA/12"
confirm-required) [3].

Key behaviours [3] `SonoraDriver.a`:

- **Open** allocates privates in `dCtlStorage`, bases itself from `VDACAddr` and the VIA
  bases (+$28000, +$26000), installs the slot interrupt handler (`SIntInstall`) and enables
  the VBL ($C0 → IER), and unblanks if blanked. **Close** blanks video and disables the VBL
  — the shipping-ROM close path sets the blank bit at $FFEF20C8, and SetInterrupt pairs
  `SIntInstall` + $C0 (at $FFEF1E64) against $40 + `SIntRemove` (at $FFEF1E2A) [4]
  (*observed*).
- **SetVidMode (depth change)**: validate mode and page 0 → gray the CLUT (gamma-corrected
  mid-gray, WaitVSync-gated stream through Ariel registers 0/1) → wait a VBL, write the depth
  to `SonoraVdColrReg`, then read-modify-write `ArielConfigReg` (mask $F8, OR in the code)
  → return the saved base address ($60B00000). **Page flipping: only page 0 exists** on PDM
  (`defPages_Sonora` = 1) [3] :599–700, :2342–2364, `DepVideoEqu.a` :1713.
- **SetEntries / DirectSetEntries**: sequential or index mode (§2.3), range-checked to the
  current depth, gamma applied from the stored table.
- **GrayPage**: repaints the dither pattern for the current depth (16 bpp uses a gray/white
  checker of 1:5:5:5 values) and reloads the CLUT.
- **SetResolution**: disables the old sRsrc in the SRT, enables the new one, reloads the
  vidParams block, blanks, reprograms monitor code + depth + CLUT, repaints gray, unblanks,
  updates PRAM. The RAM-allocation "family modes" for PDM were commented out of the driver
  ("Commented out the RAM allocation family modes for PDM", change log SM25), so only
  same-buffer families — the multiple-scan sets — switch live [3].
- **VRAM cache hack**: the 68k driver originally flushed the (copy-back) framebuffer every
  VBL on PDM; this was disabled ("Copyback VRAM just doesn't look good", change log SM7)
  once the buffer became write-through [3].

### 4.5 Mode geometry

Rowbytes are exactly `width × depth / 8` [3]. Per-mode geometry on PDM, with the frame size
at maximum depth:

| Mode (Apple name) | Pixels | Depths (PDM) | Rowbytes at 1/2/4/8/16 bpp | Frame @ max |
|---|---|---|---|---|
| Full-Page / RGB Portrait (`FP`, `RGBFP`) | 640×870 | 1, 2, 4, **8** | 80/160/320/640/– | 556,800 B |
| Rubik 12" (`GS`) | 512×384 | 1, 2, 4, 8, **16** | 64/128/256/512/1024 | 393,216 B |
| Hi-Res 13"/14" (`HR`) | 640×480 | 1, 2, 4, 8, **16** | 80/160/320/640/1280 | 614,400 B |
| VGA (`VGA`) | 640×480 | 1, 2, 4, 8, **16** | 80/160/320/640/1280 | 614,400 B |
| GoldFish 16" (`GF`) | 832×624 | 1, 2, 4, **8** | 104/208/416/832/– | 519,168 B |
| Multiple-Scan band 1 (`MSB1`) | Hi-Res family | ≤ 16 | as Hi-Res | — |
| Multiple-Scan bands 2/3 (`MSB2/3`) | GoldFish family | ≤ 8 | as GoldFish | — |

Max depths are enforced by the declaration-ROM video-parameter blocks' per-monitor
"no-VRAM max mode" triples [3] `SonoraDeclVideo.r` (nodes 2845–2875); the VGA 16-bpp cap is
the ROM's, and differs from the Developer Note's Table 3-8 (§5).

## 5. Quirks & errata

- **No VRAM, no framebuffer window, three addresses.** The frame store is allocated DRAM
  with a physical base (forced to 0), a logical mapping ($60B00000), and a scan-out base
  that is not a video register at all — it is the [HMC](hmc.md)'s configuration bit 33
  (§3.2). Software must never assume the logical and physical bases agree on any other
  machine.
- **The framebuffer must start at physical 0** — "(HMC Requirement)" in the allocator [3].
  The hardware fetch base is fixed; the OS arranges memory around it, not the other way
  round.
- **The dot clock must be running to sense.** The sense lines misread with the raster
  stopped; PrimaryInit un-blanks before sensing and re-blanks after [3] (§3.5).
- **The VBL flag is active-low.** A pending VBL drives slot IFR bit 6 to 0; the poll loop
  spins while the bit reads 1 [3] (§3.6). Code written to VIA conventions is safe; code
  assuming assert-high never synchronizes.
- **The disable literal $9F** (blank + monitor code $1F) appears in the ROM source's boot
  shutdown path but has not been located as an immediate in the shipping ROM's 68k code [3]
  [4] — the register semantics are unaffected, but that one instruction is unverified (§6).
- **Ariel config mask comment vs. mask.** The driver masks the config register with $F8
  (clearing the *bottom* 3 bits) while the source comment speaks of "the top 5 bits" — the
  comment describes what the mask *leaves*, not what it clears [3] (§2.2).
- **VGA depth cap: Dev Note vs. ROM.** The Developer Note caps built-in VGA at 8 bpp
  (307,200-byte frame) [1] Table 3-8 p. 36; the shipping ROM's PDM declaration data caps
  VGA at 16 bpp ($84) [4] (*observed*). The ROM is what production machines run.
- **Clock figures disagree between tables.** Table 2-3 prints the VGA oscillator as
  25.1570 MHz and the sound clock as 44.1584 MHz [1] p. 20, while Table 3-10's VGA dot
  clock is 25.175 MHz [1] p. 39 and the schematics show 45.1584 MHz into the AMIC [5]. The
  timing table (25.175) is the one the raster actually uses; the sound clock is not a video
  clock either way (§1.3).
- **Blank is the only knob.** DPMS-style power-down is implemented by stopping syncs
  (the blank bit); PDM has no power register — the Ardbeg-class Sonora variant's +$00A
  register does not exist here [3] (§3.7).
- **Gamma is software.** Ariel II has no gamma hardware; the driver folds gamma tables into
  the CLUT stream, and the shared named tables live in the ROM [3] [4] (§2.3).
- **The logical address $60B00000 is an LC III artifact** — chosen so the shared Sonora
  driver sees the framebuffer at Vail's VRAM base on both machines [4] (§3.2).
- **One page only.** `defPages_Sonora` = 1: page flipping is unavailable on PDM [3].
- **604 KB > largest frame, on purpose.** The allocation carries slop for QuickDraw writes
  past the frame end; the "600K/300K/68K" bucket names in the declaration data are
  historical labels, not size limits — 512×384×16 (384 KB) sits in the "600K" bucket [3]
  (§6).
- **The HDI-45's I²C pair goes to Cuda**, not the video logic — AudioVision monitor control,
  not sensing [5], [1] Table 3-9.
- **The 60.15 Hz VIA1 tick is not the VBL.** Separate AMIC timer, constant rate, no raster
  coupling [1] Figure 2-2, [3] (§3.6).
- **Sense value $00 is a broken-AMIC probe.** PrimaryInit falls back to it when a tristate
  read returns 0 — an EVT1-board inverted-sense workaround; production boards do not need
  it [3] (§3.5).
- **Early machines almost shipped with a fake VBL.** A Time-Manager pseudo-VBL
  (`hasSoftVBL`) was in the tables until October 1993 [3] (§3.6).

## 6. Open questions

1. **Ariel config-register bits above the depth field.** Only two states are ever written
   ($08 at init; the depth code OR'd into the low bits at runtime, upper bits preserved via
   the $F8 mask). Bit 3 ($08) is "master mode" per the init comment; the overlay/key bits
   are never exercised by PDM software, and no silicon documentation has been found to
   decode them [3].
2. **CLUT phase semantics.** Whether an address-register write between data bytes resets
   the R/G/B byte phase, and what the data port does through an auto-increment past entry
   $FF, are not established by any observed access pattern [3].
3. **The `$9F` disable literal** was not found as an immediate in the shipping ROM's 68k
   code [4]; whether the shipping build of the boot shutdown loads the constant differently
   or the blank-plus-$1F write happens on the PowerPC side is unresolved. The register
   semantics are not in doubt.
4. **Mode-register bits 6–5 and sense-register bits 7 and 3** are never touched by any
   known software; their behaviour is unobserved.
5. **Sync-on-green gating.** Ariel's SYNC pin drives composite sync onto green [5]; whether
   a config bit gates it — and how VGA monitors tolerate it — is unknown. VGA-mode monitors
   dislike sync-on-green, so a gate plausibly exists (perhaps automatic per monitor code
   inside the AMIC's CSYNC generation), but no source identifies one (*inferred —
   unverified*).
6. **Reset values** of the four Ariel registers, the three control registers and the
   slot-interrupt bank: unknown (§2.6 lists only what the boot code establishes explicitly).
7. **Decode extent of the control block**: whether offsets beyond +$02 at $50F28000
   (and beyond +$03 at $50F24000) decode to anything is unobserved — no software touches
   them.
8. **The 604 KB "bucket" labels** (600K/300K/68K classes in the declaration data) are
   historical naming; the allocator always grants 604 KB when any monitor is attached [3].
9. **6100/7100 board wiring.** All pin-level facts here come from the 8100 schematic set
   [5]; the smaller boards are *inferred* to match (*unverified*).
10. **The HMC bit-33 scan-base reading** (set = physical 0, clear = $00100000) rests on the
    ROM's constant state plus one third-party kernel's placement [3] [7]; direct hardware
    confirmation is lacking — see [hmc.md](hmc.md) §2.3 for the full argument.
11. **VBL phase.** The flag asserts at the start of vertical blanking by construction of the
    driver's usage; the exact line count at which the AMIC sets it is not documented.

## References

1. Apple Computer, Inc., *Developer Note: Power Macintosh Computers* (Power Macintosh
   6100/60, 6100/60AV, 7100/66, 7100/66AV, 8100/80, 8100/80AV), Developer Press, March 1994 —
   §"Audio and Video Features" pp. 5–6; §"Apple Memory-Mapped I/O Controller", §"Data Path
   Chips" and §"Ariel II Video Chip" p. 16; Figure 2-1 (block diagram) p. 13; Table 2-2
   p. 19 and Table 2-3 p. 20 (§"System Clocks"); §"CPU Bus Arbitration" Table 2-4 p. 20 and
   p. 21; §"Address Errors" p. 21; Table 2-5 p. 22; Figure 2-2 (emulated interrupt handling,
   pseudo-VIA register diagram) p. 23; §"Built-in Interface for Video Monitors" p. 35;
   Table 3-8 p. 36; Table 3-9 p. 37; Figure 3-8 p. 38; Table 3-10 p. 39; §"VRAM Expansion
   Cards" pp. 39–41.
2. Apple Computer, Inc., *Developer Note: Enhanced Power Macintosh Computers* (Power
   Macintosh 6100/66, 7100/80, 8100/100, 8100/110), Developer Press, 1994 — §"Power
   Macintosh 6100/66, 7100/80, and 8100/100", Table 1-2 (model-specific clocks) and
   §"Revised ROM and ASICs".
3. Apple Computer, Inc., Power Macintosh ROM software source, built-in video (Sonora)
   modules — `SonoraPrimaryInit.a` (init sequence :596–1000; dot-clock comment :351;
   extended sense :257–345; dot-clock-to-sense comment :655–663; 1-bpp CLUT :950–962;
   `PDMConfigTable` :1195–1258), `SonoraDriver.a` (jump table :552–572; open :300–340;
   interrupt idioms :1163–1236, :1204–1236; `SonoraSetVidMode` :599–700, :657–685;
   `SonoraWaitVSync` :2299–2325; `SonoraSetDepth` :2342–2364; change-log entries SM7, SM20,
   SM25), `SonoraDeclVideo.r` (:185–260; vidParams nodes 2845–2875), `DepVideoEqu.a`
   (:398, :419, :1158–1269, :1220–1237, :1571–1612, :1713, :2231, :2580–2637),
   `HardwarePrivateEqu.a` (:1092–1103, :1260–1305, :1755–1783), `UniversalTables.a`
   (`VideoInfoPDM`, `HMCDecoderTable`, change log `<MC4>`), `SizeMem.a` (`HMCMerge`),
   `StartInit.a`, and the shared gamma tables (`Gamma.r`), ROM-revision-5 era, 1993–1994.
4. Power Macintosh 6100/7100/8100 boot ROM, version $077D, header checksum $9FEB69B3
   (March 1994) — disassembly and byte-pattern verification of the shipping Sonora driver
   and declaration data; cited offsets: driver name string @ $2F17ED; `VideoInfoPDM`
   record @ $15FD6; Vail video record @ $15F98; vidParams blocks $2F2750–$2F28C0;
   VBL idioms $2F1E64/$2F1E2A/$2F240A/$2F2640; sense-drive writes $2F2C78–$2F2FC6;
   Ariel $08 config write @ $2F3200; `PDMConfigTable` Rubik entry @ $2F3410; CLUT
   address-register clears $2F1B0A/$2F2608; close-path blank set @ $FFEF20C8; SetInterrupt
   pair $FFEF1E64/$FFEF1E2A; gamma-table names $2F5FB4–$2F7208.
5. Apple Computer, Inc., Power Macintosh 8100 main-logic-board schematics, drawing 051-0333
   rev A — sheet 4 (HMC), sheet 5 (data path chips, "DP-VER-B"), sheet 11 (clocks), sheet 12
   (AMIC, "Apple Miscellaneous Interface Chip & Ethernet ROM"), sheet 17 (Graphics DAC,
   Ariel "ARIEL-60MHZ" U8) and sheet 18 (monitor and ADB connectors); the video path nets
   `GfxRequest*`/`GfxReset*`/`GfxWriteEven*`/`GfxWriteOdd*`/`GfxReadEven*`/`GfxReadOdd*`,
   `RawPixData<15..0>`, `DacCS*`/`DacLd*`/`DacClk`/`DacCsync*`, `CBlank*`,
   `BufAddr<0..1>`, `IOData<0..7>` and `MonID<0..2>`.
6. Apple Computer, Inc., Apple Technical Note 326 (monitor sense-line connections and ID
   codes), referenced by [1] §"AudioVision Monitor Support", p. 36.
7. Apple Computer, Inc. and Prime Time Freeware, *MkLinux DR3* kernel sources, Power
   Macintosh platform support (frame buffer placed at physical $00100000 with the HMC
   configuration's scan-base select clear), 1997.
