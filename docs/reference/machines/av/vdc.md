# The AV video digitizer — DMSD, VDC and the capture path

**Contents:**

1. [Overview](#1-overview) — what the subsystem is, which machines carry it, division of labor, clocking, the
   capture frame buffer
2. [Register file](#2-register-file) — reachability (the Cuda I²C transport), the DMSD's 25 I²C registers, the
   VDC's 17, CIVIC's video-in control bits, the video driver's control calls, reset state
3. [Behaviour](#3-behaviour) — the digitize→scale→VRAM pipeline, colour conversion, the chroma keyer, the
   VBI bypass region, the VRAM burst protocol, field processing, the field interrupt, the overlay
4. [Programming model](#4-programming-model) — the ROM's digitizer component, the boot-time programming
   sequence, standard/input/geometry/depth/picture control, the grab path, the System Enabler 088
   build
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 What the subsystem is

The Macintosh Quadra 840AV and Macintosh Centris 660AV "are the first Macintosh CPUs to provide both
video-out and video-in capabilities built into the main logic board" [1] p. 410. The video-input
half is a five-part chain soldered to the main board. From the 7-pin mini-DIN input socket to the
displayed image:

1. **TDA8708 and TDA8709 video ADCs** — "digitize the composite video waveform" [1] p. 32;
2. the **DMSD** — "decodes the result into YUV format. This common digital video format, also known
   as YCrCb, is described in CCIR Recommended Standard 601-2" [1] p. 32 — the Philips **SAA7191B**
   "Digital multistandard colour decoder, square pixel (DMSD-SQP)" [2] title page;
3. the **VDC**, Apple's name — "The VDC scales down the video image and converts its format to
   either 8-bit grayscale, 15-bit RGB, or 16-bit YUV. It stores the result in the VRAM buffer under
   the control of the CIVIC chip" [1] p. 32 — the Philips **SAA7186** "Digital video scaler" [3]
   title page (the identification, and the developer note's contrary "7169" label, are §5);
4. **[CIVIC](civic.md)**, which owns the VRAM port the VDC writes through — its timing window,
   row stride, the VDC's clock gate and the field interrupt — and whose "video RAM ... can store
   video as well as graphics" [1] p. 31; and
5. **Sebastian**, the palette DAC, which mixes the captured video with the graphics image at
   display time [1] p. 15.

The captured picture never touches main memory on its way to the screen: "Both models store video
information in RGB form in a video frame buffer separate from main memory" [1] p. 5. That frame
buffer is a region of CIVIC's VRAM at **$50200800** — VRAM offset $100800, inside the second
megabyte of CIVIC's 2 MB aperture — with a per-row stride of 1024 or 1536 bytes (§2.5, §3.7). The
CPU reads it back only when an application asks for a frame (§4.8).

The digitizer accepts "analog composite or S-video in NTSC, PAL, or SECAM format ... through an
external 7-pin mini-DIN socket. A cable adapter is provided to receive composite video from
external devices that have RCA connectors" [1] p. 32. A NuBus card on the DAV connector can also
inject component video: "Digital video in YUV format then passes to the digital audio/video (DAV)
expansion connector, where it may be picked up by a NuBus expansion card ... A slot card that uses
the DAV connector may disable the DMSD and feed its own YUV video to the VDC — for example, a slot
card containing a video decompression engine" [1] p. 32 (§6.12).

### 1.2 Machines that carry it

| Machine | Apple codename | Digitizer present | Maximum video window [1] p. 33 | Notes |
|---|---|---|---|---|
| [Macintosh Quadra 840AV](q840av.md) | Cyclone | yes | **640 × 480** | the DAV connector is on the main board, in line with NuBus slot $C [1] p. 42 |
| [Macintosh Centris 660AV](q660av.md) | Tempest | yes | **512 × 384** | the DAV connector is on the optional NuBus adapter card [1] p. 42 |

The digitizer itself is identical on both machines — the ROM carries one digitizer component for
the pair — but the driver carries a per-machine branch with one observable hardware consequence:
the 660AV skips the S-video input probe entirely (§4.4), a genuine analog-front-end difference
between the boards. The window-size difference above is the *overlay* limit; the capture limits
per monitor, VRAM size and depth are §4.5.

### 1.3 Division of labor

| Function | Owner |
|---|---|
| Analog-to-digital conversion of the input waveform | TDA8708 / TDA8709 [1] p. 32 |
| Standards decode (NTSC/PAL/SECAM), YUV 4:2:2 output, all picture controls | DMSD (SAA7191B), I²C slaves $8A/$8B (§2.3) |
| Windowing, horizontal/vertical decimation, YUV→RGB matrix, anti-gamma, chroma keyer, format packing, output FIFO | VDC (SAA7186), I²C slaves $B8/$B9 (§2.4) |
| The VRAM port: burst handshake timing, row stride, the VDC clock gate, the field interrupt, video/graphics VRAM partitioning | [CIVIC](civic.md) (§2.5, §3.7, §3.9) |
| Overlay mixing, the video CLUT bank, the α/key-colour gate | Sebastian, through its PCBR control byte at $50F30820 (§3.10) |
| The I²C bus itself — the **only** route from the CPU to either Philips part | the [Cuda](cuda.md) microcontroller, pseudo-command $22; the bus pins are on the input connector [1] Table 2-12 p. 33; see [Cuda](cuda.md) §3.10 |
| Interrupt delivery to the 68040 | CIVIC raises the slot-0 line; the [PSC](psc.md) collects it in its VIA2 window at level 2 (§3.9) |
| Address decode, machine identity, the I/O region | the platform glue (see [AV](av.md)) |

Two consequences of the middle rows are worth stating up front. First, **neither Philips part
appears anywhere in the platform's device tables** — no DecoderInfo slot, no Apple part number, no
Gestalt selector: they are reachable only through Cuda, and software that wants them must know the
two slave addresses. Second, there is **no Gestalt selector for video input at all**: presence is
discoverable only by opening the ROM's digitizer component or by reading CIVIC's `BusSize` bit back
(§2.5).

### 1.4 Clocking

The whole capture path is slaved to the *incoming video standard*, not to any Macintosh bus clock:

- The DMSD runs from "one crystal oscillator of 26.8 MHz" and produces a line-locked clock (LLC)
  at the square-pixel rates: "The YUV bus supports data rates of 780 × f_H equal to 12.2727 MHz
  for 60 Hz (NTSC-M) and 944 × f_H equal to 14.75 MHz for 50 Hz (PAL-B/G, SECAM) in 4 : 1 : 1 or
  4 : 2 : 2 formats (via the I²C-bus)" [2] p. 1. "768/640 active samples per line equals 50/60 Hz
  (SQP)" [2] p. 1 — the square-pixel property that makes a captured field map 1:1 onto the
  graphics raster.
- The VDC is specified entirely in LLC cycles: its FIFO burst timing (§3.7) is "within the next
  32 LLC cycles for 32-bit longword modes (16 LLC cycles for 16- and 24-bit modes)" [3] p. 12, and
  its input-to-output processing delay is 58 LLC in transparent mode [3] p. 28.
- 50/60 Hz field detection is automatic: the DMSD's `AUFD` bit (subaddress $0F, bit 7) selects
  "automatic 50/60 Hz field detection" [2] p. 25, and the chip switches between its two
  horizontal-timing register banks ($01–$05 for 50 Hz, $14–$18 for 60 Hz) in hardware (§2.3).
- CIVIC gates the VDC with a single control bit, `VDCClk` — **1 = clock off**, which freezes the
  VDC's VRAM writes without disturbing its internal FIFO (§3.7, §3.9).

### 1.5 The capture frame buffer

The developer note describes the VRAM split: "Each computer is delivered with two banks of VRAM
soldered in, each bank providing 0.5 MB of storage. One of the two banks can supply a graphics
screen image for monitors of small size or low color depths, letting the other bank supply live
video to be mixed with the graphic image. Two banks together can support graphics alone on monitors
that are larger or use more bits per pixel" [1] p. 33. The digitizer's frame buffer lives in that
video half, at **$50200800**; the video driver returns this address unconditionally from its
mode-set call (§2.5). Rows are written at a stride of **1024 bytes** (`VidInSize` = 0) or
**1536 bytes** (`VidInSize` = 1), selected by the programmed line width against a 992-byte edge
(§4.5).

The throughput figures the developer note quotes for the path: "The data rate for full-screen NTSC
video (640 by 480 pixels at 30 frames per second) is 18.43 MB per second. The data rate for
full-screen PAL video (768 by 576 pixels at 25 frames per second) is 22.12 MB per second. This
means that it is practical to record a video image up to one-quarter screen in size on an output
device such as a hard disk drive in real time, without data compression" [1] p. 33 — which is why
the hardware's power-on configuration is a quarter-size window (§4.2).

## 2. Register file

### 2.1 Reachability: no address, one bus, two slaves

The digitizer's control surface is unusual for a Macintosh peripheral: **nothing in it is memory
mapped.** The two Philips parts sit on the I²C bus that hangs off the Cuda microcontroller — the
developer note's connector table shows the bus pins brought out to the input socket itself
("I²C clock (Phillips serial bus)" pin 5, "I²C data (Phillips serial bus)" pin 7) [1] Table 2-12
p. 33 — and every register access is a Cuda transaction. The 68040 reaches Cuda over the VIA1
window inside the PSC ([Cuda](cuda.md) §2–3); the relevant Cuda facility is pseudo-command **$22**
(`RdWrIIC`), the I²C master, whose wire format is [Cuda](cuda.md)'s subject (§3.10).

The two slaves, both with their IICSA address-strap pin tied low:

| Part | I²C slave, write | I²C slave, read | Writable subaddresses | Strap evidence |
|---|---|---|---|---|
| DMSD (SAA7191B) | **$8A** | **$8B** | $00–$18 (25) | "SLAVE ADDRESS = 1000 101X (IICSA = LOW)"; pin 43 IICSA; "Slave address byte is 8A at pin 43 = 0 V (or 8E at pin 43 = +5 V)" [2] pp. 21–22 |
| VDC (SAA7186) | **$B8** | **$B9** | $00–$10 (17) | "SLAVE ADDRESS = 1011 100X (IICSA = LOW)"; pin 46 IICSA "LOW = B8, HIGH = BC" [3] pp. 3, 19 |

Strapping both pins low is what lets the two devices share one bus without colliding, and the
subaddress counts match the driver's shadow arrays exactly (§2.2).

The host-side shape of one transaction, as the ROM's I²C component builds it [4]: a 20-byte
parameter block with `pbCmd = $22`, the first parameter byte the **slave address** (its bit 0 the
direction — even = write, odd = read), the second an optional I²C subaddress, `pbByteCnt` = 1 or 2
accordingly, and a pointer to a Pascal-style buffer whose byte 0 is the transfer length (0 = 256).
The ROM wraps this in a four-selector Component Manager component (type `'i2c '`: read, write,
read-with-subaddress, write-with-subaddress) that transfers at most 256 bytes per call [4].

### 2.2 The register shadow

The I²C component owns a **system-heap shadow of both parts' write registers — 42 bytes: 25 for the
DMSD (subaddresses $00–$18) and 17 for the VDC (subaddresses $00–$10)** [4]. Writes go out through
Cuda *and* update the shadow; subaddressed reads are served **from the shadow** — the DMSD clamped
to $19 bytes, the VDC to $10 — and only fall through to the bus when the subaddress is out of
range. The practical contract: **neither Philips part's write registers are ever read back over the
wire**; only the two status bytes ($8B, $B9) actually hit the bus on every read [4]. The digitizer
component caches the shadow's pointer and pokes registers through it directly, so the shadow's
layout doubles as the driver's register image [4].

One protocol quirk of the VDC's I²C interface that the driver never exercises: both parts
auto-increment the subaddress when more than one data byte follows a write [3] p. 19; [2] p. 21.
Apple's driver ignores this — its block-write helper issues **one separate one-byte transaction per
subaddress**, so the 42-register boot-time initialisation is 42 Cuda round trips (§4.2) [4].

### 2.3 DMSD register map (SAA7191B, slave $8A/$8B)

Twenty-five writable subaddresses, per Table 6 of the datasheet [2] p. 22. The "Apple default"
column is the byte block the ROM's digitizer writes at every open, recovered from the disassembled
open path [4]; the "Philips reference" column is Table 7, "Recommended default values" [2] p. 31.

| Sub | Function (datasheet) | Bits | Apple default | Philips reference [2] p. 31 |
|---|---|---|---|---|
| $00 | Increment delay | IDEL7-0 | **$50** | $50 |
| $01 | H sync begin, 50 Hz | HSYB7-0 | **$30** | $30 |
| $02 | H sync stop, 50 Hz | HSYS7-0 | **$00** | $00 |
| $03 | H clamp begin, 50 Hz | HCLB7-0 | **$E8** | $E8 |
| $04 | H clamp stop, 50 Hz | HCLS7-0 | **$B6** | $B6 |
| $05 | H sync position after PHI1, 50 Hz | HPHI7-0 | **$E5** | $F4 |
| $06 | Luminance control | BYPS PREF BPSS1-0 CORI1-0 APER1-0 | **$63** | $01 |
| $07 | Hue control | HUEC7-0 | **$00** (0°) | $00 |
| $08 | Colour-killer threshold QAM | CKTQ4-0 | **$FE** | $F8 |
| $09 | Colour-killer threshold SECAM | CKTS4-0 | **$F0** | $F8 |
| $0A | PAL switch sensitivity | PLSE7-0 | **$FE** | $90 |
| $0B | SECAM switch sensitivity | SESE7-0 | **$E0** | $90 |
| $0C | Chroma gain control | COLO LFIS1-0 | **$20** | $00 |
| $0D | Standard/mode control | VTRC NFEN HRMV GPSW0 SECS | **$80** | $00 (QAM) / $01 (SECAM) |
| $0E | I/O and clock control | HPLL OEDC OEHS OEVS OEDY CHRS GPSW2 GPSW1 | **$78** | $79 (CVBS) / $7E (Y/C) |
| $0F | Control #1 | AUFD FSEL SXCR SCEN OFTS YDEL2-0 | **$98** | $91 (4:1:1) / $99 (4:2:2) |
| $10 | Control #2 | HRFS VNOI1-0 | **$00** | $00 |
| $11 | Chroma gain reference | CHCV7-0 | **$20**, then $2C/$59 per standard | $2C (NTSC) / $59 (PAL) |
| $12, $13 | unused, acknowledged | — | **$00** | $00 |
| $14 | H sync begin, 60 Hz | HS6B7-0 | **$34** | $34 |
| $15 | H sync stop, 60 Hz | HS6S7-0 | **$0A** | $0A |
| $16 | H clamp begin, 60 Hz | HC6B7-0 | **$F4** | $F4 |
| $17 | H clamp stop, 60 Hz | HC6S7-0 | **$CE** | $CE |
| $18 | H sync position after PHI1, 60 Hz | HP6I7-0 | **$E9** | $F4 |

Field semantics, from the datasheet's bit-function descriptions [2] pp. 23–26:

- **$06 luminance control** — `BYPS` (bit 7): "input mode select bit: 0 = CVBS mode (chrominance
  trap active); 1 = S-Video mode (chrominance trap bypassed)" [2] p. 25; `PREF` chrominance
  pre-filter; `BPSS1-0` the aperture bandpass characteristic; `CORI1-0` coring; `APER1-0` the
  aperture (horizontal peaking) factor. Apple's $63 = BYPS 0, PREF 1, BPSS 10, CORI 00, APER 11 —
  composite mode, pre-filter on, **aperture factor 1** (maximum luminance boost).
- **$07 HUEC** — hue control; written by the digitizer's hue control (§4.7).
- **$08/$09 colour-killer thresholds** — 5-bit fields, range "F8 to 07 (hex)" ≈ −30 dB to −18 dB
  [2] p. 23. Apple's $FE truncates to the same 5-bit value as $F8, the **−30 dB extreme** — the
  colour killer as insensitive as it goes, so weak colour is never killed. ($FE also sets a bit
  the datasheet requires to be 0 — harmless in practice, but the shadow holds it [2] p. 22.)
- **$0A/$0B switch sensitivities** — "from LOW-to-HIGH (HIGH means immediate sequence correction),
  equals FF to 00 (hex), MEDIUM equals 80" [2] p. 25; Apple biases both hard toward immediate
  correction.
- **$0C chroma gain control** — `COLO` 0 = automatic colour killer enabled; `LFIS1-0` the
  chrominance AGC loop-filter time constant, set to medium.
- **$0D standard/mode** — `VTRC` (bit 7): VCR (fast horizontal PLL time constant) vs TV; `SECS`
  (bit 0): SECAM; `NFEN`: the SAA7191B's extra outputs (RTCO/ODD/GPSW0) high-impedance or driven;
  `HRMV`: HREF generation style. Apple's $80 is literally the datasheet's own recommended
  "value for VCR mode" (its $81 sibling is "SECAM VCR mode") [2] p. 31 — the digitizer's standard
  selection ORs SECS into exactly this byte (§4.3). `NFEN` = 0 leaves RTCO/ODD/GPSW0
  high-impedance, so the board does not use them.
- **$0E I/O and clock control** — `HPLL` 0 = PLL closed (line-locked clock); `OEDC/OEHS/OEVS/OEDY`
  enable the digital outputs — Y, UV, HS/HREF, VS — which the VDC needs on its input pins;
  `CHRS` (bit 2): "S-VHS bit (chrominance from CVBS or from chrominance input): 0 = controlled by
  BYPS-bit; 1 = chrominance from chrominance input" [2] p. 25; `GPSW2/GPSW1` general-purpose
  outputs, "use is dependent on application" [2] p. 25 — on this board **GPSW1 is the analog
  input-mux select** for S-video, board knowledge the datasheet deliberately does not supply
  (§4.4).
- **$0F control #1** — `AUFD` 1 = automatic 50/60 Hz field detection (FSEL ignored); `SCEN` HCL/HSY
  outputs active; **`OFTS` 1 = 4:2:2 output format**; `YDEL2-0` luminance delay compensation.
  OFTS = 1 is mandatory here, not a preference: the SAA7186 "requires" YUV input "in 4:2:2 format
  (SAA7191B source)" [3] p. 1.
- **$10 control #2** — `HRFS` 0: "HREF is matched to the YUV output port" [2] p. 25 (correct — HREF
  feeds the VDC, not the CVBS side); `VNOI1-0` vertical noise reduction mode. Apple's $00 ("normal
  vertical noise reduction") is exactly the condition under which the VDC's odd/even field
  detection is specified to work — its nominal input is "SAA7191B with active vertical noise
  limiter" [3] p. 15 (§3.8).
- **$11 CHCV** — chroma gain; the datasheet's nominal values are $2C for NTSC and $59 for PAL
  ("nominal value for UV CCIR level with NTSC source" and the corresponding PAL level) [2] p. 31;
  SECAM "with fixed gain" [2] p. 26.
- **$01–$05 / $14–$18 H-timing** — two's-complement delay offsets with "hidden" sign bits, step
  size 2/LLC for the sync and clamp registers [2] p. 23. Apple takes the recommended values
  verbatim except $05 ($E5 vs $F4) and $18 ($E9 vs $F4) — both "H sync position", the coarse 8/LLC
  control; these two are the board's LLC/analog-delay trim (*inferred — unverified*).

**Status byte** (read, slave $8B, no subaddress) [2] Table 5 p. 21:

| Bit | Name | Meaning |
|---|---|---|
| 7 | STTC | horizontal time constant: 0 = TV (slow), 1 = VCR (fast) |
| 6 | HLCK | **1 = horizontal PLL unlocked** |
| 5 | FIDT | 0 = 50 Hz detected, 1 = 60 Hz detected |
| 4–1 | — | don't care |
| 0 | CODE | 0 = no colour, 1 = colour detected |

The digitizer reads it in two places: to build the `digiInSignalLock` flag (bit 6) and inside the
S-video input probe (§4.4) [4]. The byte matters to software: $00 reads as "PLL locked, 50 Hz, no
colour" and makes S-video selection fail; $21 (locked, 60 Hz, colour) is the "NTSC source
present" answer.

### 2.4 VDC register map (SAA7186, slave $B8/$B9)

Seventeen writable subaddresses $00–$10; $11–$1F are marked unused [3] p. 19. Write direction, bit
layout per Table 4 [3] pp. 19–20; the "Apple default" column is the block from the disassembled
open path [4] (§4.2):

| Sub | Function | D7 | D6 | D5 | D4 | D3 | D2 | D1 | D0 | Apple default |
|---|---|---|---|---|---|---|---|---|---|---|
| $00 | Formats and sequence | RTB | OF1 | OF0 | VPE | LW1 | LW0 | FS1 | FS0 | $00, then **$70** |
| $01 | Output data pixels/line | XD7–XD0 | | | | | | | | $40 |
| $02 | Input data pixels/line | XS7–XS0 | | | | | | | | $80 |
| $03 | Horizontal window start | XO7–XO0 | | | | | | | | $0C |
| $04 | Decimation filter + high bits | HF2 | HF1 | HF0 | XO8 | XS9 | XS8 | XD9 | XD8 | $89 |
| $05 | Output data lines/field | YD7–YD0 | | | | | | | | $F0 |
| $06 | Input data lines/field | YS7–YS0 | | | | | | | | $F0 |
| $07 | Vertical window start | YO7–YO0 | | | | | | | | $0F |
| $08 | Adaptive filter + high bits | AFS | VP1 | VP0 | YO8 | YS9 | YS8 | YD9 | YD8 | $A0 |
| $09 | Vertical bypass start | VS7–VS0 | | | | | | | | $00 |
| $0A | Vertical bypass count | VC7–VC0 | | | | | | | | $00 |
| $0B | Bypass high bits + misc | TCC | 0 | 0 | VS8 | 0 | VC8 | 0 | POE | $00 |
| $0C | Keyer, lower limit V | VL7–VL0 | | | | | | | | $04 |
| $0D | Keyer, upper limit V | VU7–VU0 | | | | | | | | $00 |
| $0E | Keyer, lower limit U | UL7–UL0 | | | | | | | | $04 |
| $0F | Keyer, upper limit U | UU7–UU0 | | | | | | | | $00 |
| $10 | Port / output options | 0 | 0 | 0 | MCT | QPL | QPP | TTR | EFE | $10 |

Field semantics [3] Table 6 pp. 20–22:

| Field | Meaning |
|---|---|
| RTB | anti-gamma ROM table bypass: 0 = γ = 1.4 compensation active, 1 = bypassed [3] p. 8 |
| OF1, OF0 | output field mode: 00 both fields, interlaced storage · 01 both fields, non-interlaced · **10 odd fields only** · **11 even fields only** (the single-field modes are non-interlaced) [3] p. 20 |
| VPE | **VRAM port outputs enable**: 0 = HFL/INCADR inactive, VRO three-state; 1 = port active [3] p. 20 — the capture enable |
| LW1, LW0 | first pixel position in the VRO longword: 00/01 = pixel 0 in bits 31–16; 10/11 = fill, fill, pixel 0, pixel 0 [3] p. 21 |
| FS1, FS0 | FIFO output format select, combined with EFE — the format table below [3] pp. 10–11 |
| XD9–XD0 | **pixels per line at the output**, 0…1023, at most XS ("number of XS pixels as a maximum") [3] p. 21 |
| XS9–XS0 | **pixels per line at the input**, 0…1023; longer lines are truncated, shorter lines abort at HREF fall [3] pp. 7, 21 |
| XO8–XO0 | horizontal window start ("start with 1st pixel after HREF rise"), documented range $010 to $1FF [3] p. 21 |
| HF2–HF0 | horizontal decimation filter: 000 2-tap · 001 3-tap · 010 5-tap · 011 9-tap · **100 bypassed** · 101 bypassed + 1T delay in Y · 110 8-tap · 111 4-tap [3] p. 22 — ignored when AFS = 1 |
| YD9–YD0 | lines per **output** field, 0…1023, at most YS [3] p. 21 |
| YS9–YS0 | lines per **input** field, 0…1023, "maximum = number of lines/field − 3" [3] p. 21 |
| YO8–YO0 | vertical window start: 0 = the 3rd line after the rising slope of VS; **3 = the 1st line after the falling slope of a nominal SAA7191B VS**; max $1FF [3] p. 21 |
| AFS | **adaptive filter switch**: 1 = the scaler selects both its horizontal and vertical filters itself from the estimated XD/XS and YD/YS ratios [3] pp. 7, 22 |
| VP1, VP0 | vertical data processing: 00 bypassed · 01 one-line delay · 10 filter ½(1+z⁻ᴴ) · 11 filter ¼(1+2z⁻ᴴ+z⁻²ᴴ) [3] p. 22 — ignored when AFS = 1 |
| VS8–VS0, VC8–VC0 | the **vertical bypass region** — start line (same origin as YO) and line count, 0…511 (§3.6) [3] pp. 9, 21–22 |
| TCC | U,V input representation: 0 = straight binary, 1 = two's complement [3] p. 22 |
| POE | polarity of the internally detected odd/even flag — "to compensate mis-detections" on non-standard sources [3] pp. 15, 22 |
| VL/VU/UL/UU | chroma-keyer limits, 8-bit **two's complement**; α = 1 while both U and V are inside their ranges; "Keying can be switched off by setting the lower limit higher than the upper limit" [3] p. 8 |
| MCT | monochrome / two's-complement output select: in greyscale modes, 0 = **inverse** luminance, 1 = non-inverse [3] p. 22 |
| QPL, QPP | polarity of the LNQ/PXQ qualifier outputs — pins CIVIC does not use [3] p. 22 |
| TTR | transparent data transfer: 0 = the asynchronous data-burst VRAM protocol, 1 = synchronous shift-register mode at VCLK = LLC/2 [3] pp. 12, 22 |
| EFE | extended formats enable — selects Table 3 instead of Table 2 for the FS bits [3] pp. 11, 22 |

Output format select (`EFE`/`FS1`/`FS0`) [3] Tables 2–3 pp. 10–11:

| EFE | FS1 | FS0 | Output format |
|---|---|---|---|
| 0 | 0 | 0 | **RGB 5-5-5 + α; 2 × 16 bit/pixel; 32-bit word; RGB matrix ON** — the 16-bit video-in format |
| 0 | 0 | 1 | YUV 4:2:2; 2 × 16 bit/pixel; 32-bit word; matrix off |
| 0 | 1 | 0 | YUV 4:2:2 video test mode; 1 × 16 bit/pixel; 16-bit word |
| 0 | 1 | 1 | **8-bit monochrome; 4 × 8 bit/pixel; 32-bit word** — the 8-bit video-in format |
| 1 | 0 | 0 | RGB 5-5-5 + α; 1 × 16 bit/pixel; 16-bit word; + transparent format |
| 1 | 0 | 1 | YUV 4:2:2 + α; 1 × 16 bit/pixel; 16-bit word; + transparent format |
| 1 | 1 | 0 | RGB 8-8-8 + α; 1 × 24 bit/pixel; 24-bit word; + transparent format |
| 1 | 1 | 1 | 8-bit monochrome; 2 × 8 bit/pixel; 16-bit word; + transparent format |

In the EFE = 0, FS = 00 format Apple uses, the longword on VRO(31–0) is **α, R4-R0, G4-G0, B4-B0**
for pixel *n* in bits 31–16 and the same for pixel *n+1* in bits 15–0 [3] p. 10 — two Mac
`1-5-5-5` ARGB pixels per longword, leftmost pixel in the high half, with the α bit at bit 15 of
each 16-bit pixel.

**Status byte** (read, slave $B9, no subaddress) [3] Table 5 p. 20:

| Bit | Name | Meaning |
|---|---|---|
| 7–4 | ID3–ID0 | software/mask version; the only tabulated value is 0001 = version 1 |
| 3–2 | — | 0 |
| 1 | OEF | field sequence: 0 = even field detected, 1 = odd field detected |
| 0 | SVP | state of the VRAM port: 0 = HFL/INCADR inactive, 1 = active (i.e. whether VPE has taken effect) |

The digitizer reads this byte verbatim and hands it to the caller [4]. A byte answering $10 (even
field, port idle) before the port is enabled and $13 (odd field, port active) once VPE = 1 is the
documented-consistent behaviour; a zero high nibble would claim an undocumented revision.

### 2.5 CIVIC's video-in control bits

The CIVIC-side registers the capture path touches live in CIVIC's serial register window at
**$50036000** (one bit per longword; full conventions in [CIVIC](civic.md)). The subset:

| Name | CIVIC offset | Absolute | Width | Semantics |
|---|---|---|---|---|
| VDCInt | $008 | $50036008 | 1 | VDC field-interrupt flag — **active LOW** (bit 0 = 0 means pending) |
| VDCClr | $00C | $5003600C | 1 | interrupt acknowledge: write 0, then 1 |
| VDCEnb | $010 | $50036010 | 1 | 1 = enable the VDC field interrupt |
| VidInSize | $014 | $50036014 | 1 | capture row stride: **0 = 1024, 1 = 1536 bytes** |
| VDCClk | $018 | $50036018 | 1 | **1 = VDC clock OFF** (blocks the VDC's VRAM writes) |
| VSCDivide | $02C | $5003602C | 1 | video-in shift-clock divide (§6.8) |
| VRAMSize | $040 | $50036040 | 1 | 1 = the optional second MB of VRAM is installed |
| BusSize | $04C | $5003604C | 1 | **0 = 32-bit (video-in enabled); 1 = 64-bit graphics-only** — the master switch |
| VInHAL | $1C0 | $500361C0 | 12 | video-in H active start (the video driver's) |
| VInHFPD | $200 | $50036200 | 2 | video-in early H front-porch delay — always programmed 0 (§6.9) |
| VInDoubleLine | $208 | $50036208 | 1 | CIVIC II hardware line doubling: **0 = enable, 1 = disable** |
| VInHFP | $240 | $50036240 | 12 | video-in H front porch |
| VInVAL | $5C0 | $500365C0 | 12 | video-in V active start |
| VInVFP | $600 | $50036600 | 12 | video-in V front porch |

The geometry registers (VInHAL/VInHFP/VInVAL/VInVFP) are written only by the video driver — the
digitizer component never touches a multi-bit CIVIC register [4]; the two components meet through
the driver's control calls instead.

**The video driver's control/status calls** — the digitizer's only route into CIVIC's geometry
[4]:

| csCode | Control | Status |
|---|---|---|
| 136 ($88) | `cscSetVideoIn` — **not implemented** | `cscGetVideoIn` — csMode 0 if enabled, 1 if disabled |
| 137 ($89) | `cscSetVidInMode(csMode, csPage → csBaseAddr)` | `cscGetVidInMode` |
| 138 ($8A) | `cscSetVidInRect(csRect → csBaseAddr)` | `cscGetVidInRect` |
| 139 ($8B) | — | `cscGetCompCapab` — the component-capability record |

The mode-set call returns **csBaseAddr = $50200800 unconditionally**, and rejects any csMode other
than 8 or 16 — the capture path has exactly two depths. Note the asymmetry: there is no
`SetVideoIn` control call at all; video-in is enabled implicitly by switching the display to a
mode whose bus size is 0.

Sebastian's control byte (the PCBR at $50F30820) carries the overlay state [4]:

| Bit | Meaning |
|---|---|
| 2–0 | graphics depth code |
| 3 | **video-in depth: 0 = 8 bpp (greyscale), 1 = 16 bpp** |
| 4 | video-in enable |
| 5 | convolution (flicker filter) enable |
| 6 | CLUT bank select: 0 = graphics, 1 = video |
| 7 | **overlay enable** |

### 2.6 Reset state

- **VDC**: after power-on reset the part is inactive — VPE is 0 and "the FIFO control is set
  'empty'": FIFO contents undefined, VRO high-impedance, INCADR HIGH, **HFL LOW until the VPE bit
  is set to 1**, subaddress $10 = $00 and VPE in subaddress $00 = 0 [3] p. 12. The digitizer's
  first VDC write leaves $00 = $00 — matching the reset state — and the $70 that follows is what
  actually starts the part [4].
- **DMSD**: no power-on register values are documented [2]; immaterial in practice, because the
  driver writes all 25 subaddresses at every open before it depends on any of them [4].
- **CIVIC**: the reset values of the video-in bits are not established; what is pinned is that
  `BusSize` and `VRAMSize` **must read back** — the digitizer samples both at open (§4.1) and
  resynchronizes on `BusSize` at the top of every selector call [4].

## 3. Behaviour

### 3.1 The pipeline end to end

```
composite ──► TDA8708/8709 ADCs ──► DMSD SAA7191B ──16-bit YUV 4:2:2 (LLC, CREF, HREF, VS)──┐
S-video   ──► (same ADCs, muxed)     NTSC/PAL/SECAM decode                                   │
                                                                                           ▼
                                           SAA7186 "VDC": window (XO/YO), decimate (HF/VP),
                                           chroma keyer → α, YUV→RGB (CCIR 601), anti-gamma,
                                           format pack (FS) ──► 16-word × 32-bit FIFO
                                                                                           │
                           VRO(31–0) ◄── VRAM output port, burst protocol ── HFL, INCADR out,
                           VCLK in, VOEN in ◄── CIVIC as the external memory controller
                                                                                           ▼
                      CIVIC VRAM: capture frame buffer at $50200800, rows 1024/1536 bytes
                                                                                           ▼
                      Sebastian overlays it onto the graphics image (PCBR bits 4/7, video CLUT)
```

### 3.2 Digitization

The DMSD's job is to turn the sampled waveform into a standards-locked 4:2:2 component stream. Its
horizontal PLL is line-locked (HPLL = 0: the sampling clock is regenerated from the input's own
horizontal sync), with a VCR/TV time-constant bit (VTRC) that the software exposes as the
VTR/broadcast source hint (§4.7); 50/60 Hz detection (AUFD) is automatic and switches the
horizontal-timing register banks in hardware, so no driver code reprograms $01–$05 vs $14–$18 per
standard. Luminance processing carries the chrominance trap (composite), or bypasses it
(S-video, BYPS = 1), with a programmable aperture/peaking filter whose `APER` settings the
driver's sharpness control indexes (§4.7). Chrominance processing runs an AGC loop (LFIS) toward
the CHCV reference gain, a colour killer with the two threshold registers, and the standard
sequence-correction machinery (PLSE/SESE sensitivities). The output is the 4:2:2 square-pixel YUV
bus the VDC requires [3] p. 1, with all four digital outputs (Y, UV, HREF, VS) enabled — the
OEDC/OEHS/OEVS/OEDY bits of $0E — because those are the VDC's input pins.

### 3.3 Scaling

The VDC windows the incoming field and decimates it: "The CMOS circuit SAA7186 scales and filters
digital video data to randomly sized picture windows" [3] p. 1, with two-dimensional processing —
horizontal decimation filters (2/3/4/5/8/9-tap or bypassed) and a vertical processing unit with two
line memories ("Line memories in Y path and UV path to store two lines, each with 2 × 768 × 8 bit
capacity" [3] p. 1). Two structural limits:

- **The window parameters are also a pan/zoom.** "The scaling parameters can be used to perform a
  panning function over the video frame/field" [3] p. 9 — XO/YO place the window, XS/YS vs XD/YD
  set the ratio.
- **The hardware cannot upscale.** XD is "number of XS pixels as a maximum" and YD at most YS
  [3] p. 21; the part "Processes maximum 1023 pixels per line and 1023 lines per field" [3] p. 1,
  and sequential input data "are limited to maximum 768 active pixels per line if the vertical
  filter is active" [3] p. 7 — 768 being exactly the PAL source width the digitizer reports
  (§4.5). The consequence is visible in the component's capability flags: it advertises shrink,
  quarter and sixteenth, but **no stretch, double or quad** (§4.1).

Apple runs the scaler with **AFS = 1** — "filter characteristics are selected by the scaler" itself
from the estimated ratios [3] p. 22 — so the HF/VP fields of $04/$08 are inert in the shipped
configuration, and the filter choices are made by hardware policy, not by Apple's register values.

### 3.4 Colour conversion, gamma and levels

The YUV→RGB matrix is switchable and CCIR 601 quantized [3] p. 8:

```
R = Y + 1.375 V
G = Y − 0.703125 V − 0.34375 U
B = Y + 1.734375 U
```

Anti-gamma correction rides at the matrix output: "ROM tables are implemented at the matrix output
to provide anti-gamma correction of the RGB data. A curve for a gamma of 1.4 is implemented" [3]
p. 8 — bypassable by RTB (subaddress $00 bit 7), which is exactly what the digitizer's private
selector 352 toggles (§4.7). Levels follow CCIR 601: **16 = black, 235 = white**; 8-bit RGB is
clipped to 0/255, the 5-bit RGB components of the packed format are produced by **truncation**
from 8 bits, and fill bytes inserted for unused longword positions are 0 in RGB formats, 128 for
straight-binary U/V and 255 in 8-bit greyscale [3] p. 9. The processing delay through the part is
58 LLC in transparent mode [3] p. 28 — the offset the window starts "have to be set according to"
[3] p. 21.

### 3.5 The chroma keyer and the α bit

"The keyer generates an alpha signal to achieve a 5-5-5 + α RGB alpha output signal. Therefore,
the processed UV data amplitudes are compared with thresholds set via I²C-bus (subaddresses 0C to
0F). A logical 1 signal is generated if the amplitude is inside the specified amplitude range,
otherwise a logical 0 is generated" [3] p. 8. In the FS = 00 longword format the α bit lands on
VRO31 for the even pixel and VRO15 for the odd pixel [3] p. 10 — bit 15 of each 16-bit video
pixel, the same position as the alpha/overlay bit of a Mac 16-bpp `1-5-5-5` pixel. That is the
hinge of the overlay: in 16-bpp video-in the hardware delivers a **per-pixel alpha bit**, and the
digitizer reports itself as an alpha-type digitizer; at 8-bpp video-in (monochrome FS = 11, no α)
keying must be done by colour index instead (§3.10, §6.6).

**Apple ships with the keyer off**, using the datasheet's own documented idiom: the lower limit
(+4) exceeds the upper (0) for both U and V, so no amplitude is ever "inside" and **α is
constantly 0** [3] p. 8; [4]. The shipped overlay is therefore gated by the whole-window PCBR
bits, not per pixel (*inferred — unverified*: treating α = 0 as the opaque-video case is the state
in which the shipped overlay demonstrably works).

### 3.6 The VBI bypass region

The input field divides into two vertical regions — the **bypass region** (VS, VC) and the
**scaling region** (YO, YS) — and "the two regions can be programmed via I²C-bus, whereby regions
should not overlap (active region overrides the bypass region)" [3] p. 9. In the bypass region
"data are not scaled and independent of I²C-bits FS1, FS0 the output format is always 8-bit
grayscale (monochrome). The SAA7186 outputs all active pixels of a line, defined by the HREF input
signal ... This can be used, for example, to store videotext information in the field memory"
[3] p. 9. The digitizer uses it as its VBI capture window: when a requested capture rectangle
reaches into the vertical blanking interval, the driver programs VS/VC for those lines and
shortens the scaled region (§4.5). Two consequences follow straight from the datasheet: a VBI
capture always arrives as **unscaled 8-bit greyscale at full line width**, regardless of the
picture depth, and the line origin of VS/YO is counted from the third line after the rising slope
of the VS pin, with the value 3 meaning the first line after the falling slope of a nominal
SAA7191B VS [3] p. 21 — the digitizer's two-line top adjustment is plausibly its compensation for
exactly that origin (*inferred — unverified*).

### 3.7 The VRAM output port and CIVIC's handshake

The bridge from the scaler to memory is a **16-word × 32-bit output FIFO** feeding the VRAM output
port — "the buffer between the video data stream and the VRAM data input port. Resized video data
are buffered and formatted" [3] pp. 1, 12. The asynchronous data-burst protocol (TTR = 0) uses
four pins:

- **HFL** (half-full flag) rises when the FIFO holds at least 8 words; by raising HFL the part
  "requests a data burst transfer by the external memory controller, that has to start a transfer
  cycle within the next 32 LLC cycles for 32-bit longword modes (16 LLC cycles for 16- and 24-bit
  modes)" [3] p. 12. At end of line the part fills the FIFO with fill pixels until half-full and
  raises HFL anyway, so partial lines always drain.
- **INCADR** is the address-control signal; its rising edge means **end of line** when HFL = 1
  (request line-address increment) and **end of field/frame** when HFL = 0 (request line *and*
  pixel address reset) [3] p. 12.
- **VCLK** clocks data out of the FIFO; **VOEN** (active-low) enables the VRO drivers. The subtle
  clause the capture path depends on: "If VCLK pulses are applied during VOEN = HIGH, the outputs
  remain inactive, but the FIFO register accepts the pulses" [3] p. 12 — which is why CIVIC can
  freeze the buffer with the VDCClk gate **without corrupting the FIFO's state**: the burst is
  merely suspended, and a later unfreeze resumes cleanly.

CIVIC is the "external memory controller" of that specification: it provides the burst clock,
honours HFL/INCADR, increments the row address, and lands rows at the stride VidInSize selects —
this is the hardware behind the "stores the result in the VRAM buffer under the control of the
CIVIC chip" sentence of the developer note [1] p. 32.

### 3.8 Field processing

Field phase is detected from the falling edge of VS and reported in the status byte's OEF bit;
"OEF bit can be stable 0 or 1 for non-interlaced input frames or non standard input signals VS
and/or HREF (nominal condition for VS and HREF — SAA7191B with active vertical noise limiter). A
free-running odd/even flag is generated for internal field processing if the detection reports a
stable OEF bit" [3] p. 15 — and POE exists to invert that flag when a source is misdetected
[3] p. 15. Apple leaves POE = 0 and runs the DMSD with the vertical noise limiter active ($10 =
$00), i.e. exactly the nominal condition [4]; [2] p. 25.

The OF field-mode bits control what reaches memory [3] p. 15:

- **OF = 00 (both fields, interlaced):** "two INCADR/HFL sequences are generated in each
  qualified line; additionally, an INCADR/HFL sequence after the vertical reset sequence of an
  odd field" — the double line-increment is how a field's lines land on alternate VRAM rows, and
  the extra sequence after an odd-field reset is the half-line offset. "Thereby, the scaled lines
  are automatically stored in the right sequence."
- **OF = 01 (both fields, non-interlaced):** consecutive rows.
- **OF = 10 / 11 (odd only / even only):** one field is ignored entirely; "no line increment
  sequence is generated, the vertical reset pulse is only generated" — consecutive rows, at half
  the vertical source resolution. Apple's power-on default is 11 (even fields only), which is half
  of the recipe for the correctly-proportioned quarter-size power-on window (§4.2).

Register writes latch at the field boundary: "The internal control registers are updated with the
falling edge of the VS signal" [3] p. 15. The digitizer honours this by construction — it clears
VPE (bit 4 of $00) before reprogramming geometry, writes the new values, and sets VPE again
(§4.8) — precisely so a mid-field change cannot tear the picture.

### 3.9 The field interrupt

CIVIC latches **one interrupt per captured field** and presents it through its VDCInt bit
($50036008, **active low** — bit 0 = 0 means pending, opposite to the vertical-blanking flag's
active-high convention), acknowledge by writing VDCClr ($5003600C) 0 then 1, and enable through
VDCEnb ($50036010) [4]. What the hardware event is, is settled to the extent the datasheet allows:
the one signal the VDC emits exactly once per output field is the **end-of-field sequence on
INCADR** — HFL = 0 at the rising edge, after the last line's data burst [3] p. 12 — so the
interrupt is CIVIC latching that (*inferred — unverified* at CIVIC's side: whether CIVIC wires
the pin directly or derives the event from its own address generator wrapping, and hence whether
a partially transferred field can raise it, is §6.1). The end-of-line pulses (HFL = 1) drive
CIVIC's row increment and are not visible to software.

Delivery is through **slot 0** — the same line as the vertical-blanking interrupt: the digitizer
installs its handler with the Slot Manager at priority 200, below CIVIC's own handler at priority
255, and CIVIC's handler explicitly declines to service the interrupt when its own
vertical-blanking flag is clear, letting the Slot Manager walk on to the digitizer's element
(*observed* in the disassembled arm sequence and the driver's interrupt prologue [4]). The line
reaches the 68040 as level 2 through the PSC's VIA2 window — the on-board-video/slot bank bit
(see [PSC](psc.md) §2.8).

### 3.10 The overlay

At display time Sebastian mixes the captured video with the graphics image [1] p. 15. The software
side of the mixing is the PCBR byte of §2.5: video-in enable (bit 4), overlay enable (bit 7), the
video-in depth (bit 3), and the CLUT bank select (bit 6). On a depth/mode change the video driver
selects the **video CLUT bank** and loads it with a **32-entry grey ramp** — ascending in 16-bpp
video-in, **descending (inverted) in 8-bpp video-in** — indexed by the five MSBs of each channel
[4]; 8-bpp video-in is greyscale (the FS = 11 format, matrix off [3] p. 10), so the ramp is the
whole of its colour behaviour. In 16-bpp the mixing key is the α bit the VDC delivers (§3.5); at
8-bpp graphics the digitizer is a key-colour digitizer and the driver maintains a 256-byte
key-colour map and a saved copy of the graphics CLUT to implement it (§6.6).

One display-side interaction is worth naming here because it costs a feature: "Apple convolution
is not supported in more than 256 colors or **when a video input window is active**" [1] p. 410 —
the flicker filter and the video-in window are mutually exclusive.

## 4. Programming model

The shipped software is two Component Manager components living in the boot ROM, plus their System
Enabler 088 replacements. Everything in this section is recovered from the disassembled ROM and
Enabler resources [4]; selector names follow the 1994 QuickTime component headers the driver was
compiled against.

### 4.1 Software inventory and gating

| Resource | Type/ID | ROM file offset | Maps at | Length |
|---|---|---|---|---|
| VDig component | `'thng'` −16727 | $1A51A0 | $409A51A0 | $38 |
| VDig code | `'code'` −16729 | $1A5200 | **$409A5200** | $730C (29452 B) |
| I2C component | `'thng'` −16728 | $1AC530 | $409AC530 | $38 |
| I2C code | `'code'` −16730 | $1AC5A0 | $409AC5A0 | $7AC (1964 B) |

Both components: manufacturer `'appl'`, subtype `'m3tv'`; the digitizer is type `'vdig'`, the I2C
one type `'i2c '` [4]. The digitizer **refuses to register** unless all of [4]:

- `Gestalt('mach')` is one of **43, 60, 78, 79** — 78 = Quadra 840AV, 60 = Centris 660AV; 43 and
  79 are unnamed pre-release IDs grouped with the 840AV and 660AV respectively;
- the ROM version word at ROMBase+$08 is **$077D** and the ROM release at ROMBase+$12 is
  **$10F3** (the identity of the shipping 2 MB AV ROM image);
- a scan of the machine's component table finds the longword **$409A5200** — the ROM address of
  the ROM's own digitizer code resource.

Open allocates a 940-byte globals block, caches the I²C shadow pointer, **holds the first $2E
bytes wired** (the interrupt handler touches them at interrupt time), reads CIVIC's `BusSize`
and `VRAMSize` back, opens the I²C component, writes both register blocks (below), un-gates the
VDC clock, installs the slot-0 interrupt handler, runs the field-arrival probe, and primes the
geometry from the standard's active-source rectangle [4].

The component's self-description, from its `GetDigitizerInfo` handler [4]:

| Field | Value |
|---|---|
| vdigType | **1 (alpha-type)** when graphics is 16 bpp; **3 (key-colour-type)** at ≤ 8 bpp |
| inputCapabilityFlags | $00003307 = NTSC, PAL, SECAM, composite, S-video, colour, black-and-white — **all three standards**; no genlock, no component/RGB input |
| outputCapabilityFlags | $0F68631F — depths 1/2/4/8/16, **shrink, mask, quarter, sixteenth**, blend, hardware DMA, hardware play-through, key colour, async grabs and the compression bits; **no stretch, double or quad** |
| maxDestHeight / maxDestWidth | 576 / 752 |
| blendLevels | 2 at 16-bpp graphics, 8 at ≤ 8 bpp |

Two of those fields depend on CIVIC at call time: when video-in is not currently enabled
(`BusSize` = 1, the driver's "TV mode" flag clear), the GDevice handle is forced to 0 and
`digiOutDoes32` is OR-ed into the output flags [4].

### 4.2 The open path — the golden programming sequence

Verbatim structure of the disassembled open path [4] (component-relative offsets in the `'code'`
−16729 resource):

```
; 1. attach to the I²C component and its 42-byte shadow
FindNextComponent('i2c ','m3tv','appl'); OpenComponent
shadow = GetComponentRefcon(i2c)

; 2. CIVIC: enable hardware field doubling
VInDoubleLine ($50036208) := 0            ; direct longword write

; 3. DMSD: fill the 25-byte shadow, then push one byte per subaddress
shadow[$00..$18] := 50 30 00 E8 B6 E5 63 00 FE F0 FE E0 20 80 78 98
                   00 20 00 00 34 0A F4 CE E9
SendI2CBlock($8A, &shadow[$00], lastSub=$18)   ; 25 separate one-byte transactions

; 4. VDC: fill the 17-byte shadow, push, then start the part
shadow[$00..$10] := 00 40 80 0C 89 F0 F0 0F A0 00 00 00 04 00 04 00 10
SendI2CBlock($B8, &shadow[$00], lastSub=$10)   ; 17 transactions
SendIIC($B8, $00, $70)     ; RTB=0 (anti-gamma on), OF=11 (even fields only),
                            ; VPE=1 (VRAM port ON), LW=00, FS=00 (RGB 5-5-5 + α)

; 5. self-tests of colour space and field preference
self.VDSetInputColorSpaceMode(1)   ; vdDigitizerRGB; on error fall back to (0) BW
self.VDSetFieldPreference(0)       ; vdUseAnyField

; 6. CIVIC: un-gate the VDC clock
VDCClk ($50036018) := 0             ; SUPPRESSED unless video-in is enabled (§4.6)
```

Decoded, Apple's power-on VDC configuration is: capture the **even field only** of a **640 × 240
per-field** window starting at source pixel 12 / field line 15, scale it **2:1 horizontally and 1:1
vertically** (XD = $140 = 320, XS = $280 = 640, YD = YS = $F0 = 240), let the chip pick its own
filters (AFS = 1), run the **YUV→RGB matrix and the γ = 1.4 anti-gamma ROM**, emit **1-5-5-5
ARGB, two pixels per longword**, with the **chroma keyer off** and the **VBI region off**, over
the asynchronous data-burst VRAM port [3]; [4]. Even-field-only already halves the vertical
resolution, so only the horizontal 2:1 is needed — a correctly proportioned **320 × 240 quarter
image**, matching the component's advertised quarter capability and the developer note's
quarter-screen practical capture [1] p. 33. The two self-calls rewrite FS and OF immediately
afterwards, so the even-field-only default does not survive to the first grab [4].

The one deliberate under-range value: Apple's XO = 12 (from $03 = $0C) is **below the datasheet's
documented minimum of $010 = 16** for the horizontal window start [3] p. 21. Either the minimum is
conservative (it covers the internal pipeline delay) or the $0C is a deliberate four-pixel lead
compensating the DMSD's HREF timing (*inferred — unverified*); it is moot in practice because the
first geometry call recomputes XO (§4.5).

### 4.3 Standard selection

`VDSetInputStandard` writes exactly two registers [4]:

```
NTSC:  DMSD $11 := $2C     ; CHCV — the datasheet's nominal NTSC CCIR chroma gain
PAL:   DMSD $11 := $59     ; CHCV — the PAL value
SECAM: DMSD $11 stays $59, and DMSD $0D := old | $01   ; SECS
       (all standards)      DMSD $0D refreshed
```

Both CHCV values are the datasheet's own Table 7 entries — "nominal value for UV CCIR level with
NTSC source" $2C and the corresponding PAL level $59 [2] p. 31 — and SECAM's fixed-gain operation
[2] p. 26 is why the driver leaves CHCV at the PAL value for SECAM and only adds the SECS bit.
The VTR/TV bit in the same register ($0D) is what the PLL-filter-type selectors manipulate, which
is how the broadcast/VTR source hint reaches the hardware [4].

### 4.4 Input selection — composite vs S-video

`VDSetInput` implements the composite/S-video switch as a **probe, then commit** [4]:

```
composite: DMSD $0E: CHRS := 0, GPSW1 := 0     ; analog mux → composite
           DMSD $06: BYPS := 0                ; chroma trap active
S-video:   DMSD $0E: GPSW1 := 1               ; analog mux → S-video
           DMSD $06: BYPS := 1                ; chroma trap bypassed
           wait ~0.5 s (30 ticks)
           status := DMSD status byte ($8B)
           if PLL unlocked (HLCK) or colour detected (CODE):
               DMSD $0E: CHRS := 1            ; chroma from the C input — commit
           else:
               DMSD $06: BYPS := 0            ; no signal — fall back to composite
```

The register semantics behind the dance are the datasheet's: BYPS selects composite vs S-video mode
(trap active/bypassed) [2] p. 25; CHRS routes chrominance from the dedicated C input rather than
out of the composite path [2] p. 25; and GPSW1 is an application-defined output pin [2] p. 25
that this board uses as the analog input-mux select. The commit-after-probe order exists because
with CHRS = 0 the chip is still demodulating chroma from the composite path, so the status byte
reports on the *composite* interpretation of whatever is plugged in — the driver commits the
C-input routing only once the status byte shows a live signal [4].

**The 660AV skips the probe entirely** — no BYPS set, no status read, straight to the S-video
configuration (*observed* as the one use of the open path's machine-class byte, 0 for the 840AV
and 1 for the 660AV [4]): the Centris board's analog front end does not need the auto-detect, a
genuine per-machine hardware difference. RGB/component input is not supported.

### 4.5 Geometry

The rectangles the component reports to applications — the only place the capture geometry is
stated in absolute pixels (left, top, right, bottom) [4]:

| Call | NTSC | PAL / SECAM |
|---|---|---|
| GetMaxSrcRect | (0, 0, 656, 510) | (0, 0, 768, 614) |
| GetActiveSrcRect | **(16, 30, 656, 510) = 640 × 480** | **(16, 38, 768, 614) = 752 × 576** |
| GetVBlankRect | (0, 0, 656, 29) | (0, 0, 768, 37) |

`VDSetDigitizerRect` is the clipping engine, and it is where the VDC and CIVIC meet [4]:

1. Validate; subtract 2 from `top` (the vertical-origin compensation of §3.6).
2. If the rect reaches into the VBI (top ≤ 29 NTSC / 37 PAL): program the **VDC vertical bypass
   region** ($09 = VS start, $0A = VC count, $0B high bits) for the VBI lines, then force the
   scaled region's top to 30/38 and shorten. Otherwise zero VS/VC — region off. Those VBI lines
   come back **unscaled, full-width, 8-bit greyscale** whatever the picture depth (§3.6).
3. Vertical: halve top and height into **field** lines → VDC $06/$07/$08 (YS input lines, YO
   window start); the vertical parameters are per field because the scaler "operates on fields".
4. Horizontal: width → **XS** (input pixels, $02) and start → **XO** ($03/$04), with three
   fix-ups: a snap of XS to the destination width when the ratio would land just off 1:1 (which
   in adaptive mode would beat against the pixel grid — *inferred*); an 8-pixel narrowing for
   full-width standard rects; and a clamp of start+width to the standard's full width (640/752),
   then start to $1FF.
5. Store the adjusted rect; re-derive CIVIC's stride bit: **VidInSize = 1 iff the byte-per-line
   exceeds 992** — 992 bytes = 496 pixels in 16 bpp, the exact edge between the 1024- and
   1536-byte rows.

The destination side — XD ($01/$04) and YD ($05/$08) — is written as its own group by the
destination-size selectors [4]; XD/XS and YD/YS *are* the scaling ratios.

**CIVIC's raster-side geometry** (the video driver's, reached through `cscSetVidInRect`) maps the
same rect onto the *graphics* timing already programmed, in the order VidInSize, VInHFP, VInHFPD,
VInHAL, VInVFP, VInVAL, each write wrapped in a vertical-sync wait at interrupt level ≥ 2 [4]:

```
vertical, progressive:    VInVAL = VAL + (top << 1)     VInVFP = VAL + (bottom << 1)
vertical, interlaced:     VInVAL = (VAL + top) & ~1     VInVFP = (VAL + bottom) & ~1
16-bpp video-in, or 8-bpp video-in over 16-bpp graphics:
                          VInHAL = HAL + left − 1        VInHFP = HAL + right − 1
8-bpp video-in over 1/2/4/8-bpp graphics:
                          VInHAL = HAL + (left>>1) − 1  VInHFP = HAL + (right>>1) − 1
```

(HAL/HFP are read back from CIVIC rather than taken from the mode table, because a video-in mode
change may have altered them; the −1 is the driver's own "fudge factor".) Rects are clipped
against the display's own porches and returned corrected; with video-in off, the window is parked
outside the raster at VInHAL = $FF0, VInHFP = $FF8, VInVAL = $FF0, VInVFP = $FF8 [4]. Legality
checking rejects negative coordinates with a (−1,−1,−1,−1) rect and inverted rects with a zero
rect, and — in 8-bpp video-in only — forces `left` even and the width a multiple of 4 [4].

**Capture size limits** come from the video driver's table of maximum video-in rectangles, indexed
by monitor sense code, VRAM size and video-in depth, "assum[ing] that the maximum video in window
to be displayed is FF PAL: 768,576" [4]. The salient leaves: at 8 bpp the full PAL 768 × 576
window is allowed with 2 MB VRAM on every monitor that can show it; at 16 bpp with only 1 MB of
VRAM the driver computes the height from the width against a ~512 KB video-in budget (512 tall up
to 512 wide, 340 at full width) — and past 338 captured lines (NTSC) / 510 (PAL) on a 1 MB machine
the digitizer captures **fields** and doubles lines rather than frames [4]. These are the
overlay-window limits; the developer note's headline numbers (640 × 480 on the 840AV, 512 × 384
on the 660AV) [1] p. 33 describe the shipped configurations' maximum displayable window.

### 4.6 Depth and mode

`cscSetVidInMode` (the driver control call, csCode 137) is the depth switch [4]:

1. reject any page but 0, reject the call when the current graphics mode is not video-in-capable,
   reject any depth but 8 or 16; return csBaseAddr = $50200800 unconditionally;
2. if the overlay is already on at the requested depth, do nothing;
3. check the mode table's maximum video-in depth, then the size table against the current rect;
4. clear the overlay bit in Sebastian's PCBR (no garbage while reprogramming);
5. set the video-in depth bit, then program CIVIC: `BusSize := 0` always — the 32-bit VRAM bus is
   the master switch — the shift-clock divide bits, row words, base address and pipeline registers;
6. set the overlay bit, write PCBR; reprogram the rect;
7. select the **video CLUT bank** and load the 32-entry ramp (§3.10).

The capability table marks, per monitor and VRAM size, which graphics depths can carry video-in
and to what video depth — for the shipped 640 × 480 monitor entries, graphics at 1/8/16 bpp
carries 8-or-16-bpp video, at 32 bpp none, exactly the bus-size rule [1] Table 2-13 p. 34; [4].
**No video-in exists at 32-bpp graphics**, because that mode's bus size is 1: the driver's
depth-switch path derives its video-in-enabled flag from the mode's bus size, and the digitizer
re-derives the same thing by reading BusSize back at the top of every selector [4].

### 4.7 Picture controls

| Control | Selectors | Default | Hardware target | Mechanism |
|---|---|---|---|---|
| brightness | 12/13 | $7FFF | **DMSD $14/$15/$16/$17** — the 60 Hz H-sync/H-clamp positions | moves the sync reference, not the clamp window (below) |
| contrast | 14/18 | $0000 | **none** | both selectors return −2201 (unimplemented) |
| hue | 15/19 | $7FFF | DMSD $07 HUEC | HUEC = (v >> 8) − $7F |
| sharpness | 16/20 | $A000 | DMSD $06 luminance control | 16-entry aperture table, indexed by (v >> 13), +8 when BYPS (S-video) is set, preserving bit 7: `00 03 50 42 43 53 73 63 80 81 82 83 C0 C1 C2 C3` |
| saturation | 17/21 | $2C00 | DMSD $11 CHCV | CHCV = v >> 8 — the default is literally the NTSC CCIR gain ($2C) |
| black/white level | — | $FFFF / $0000 | none | reported by GetVideoDefaults only; no selectors |
| PLL filter type | 41/42 | — | DMSD $0D VTRC | the VTR/broadcast hint |

Brightness is the structural surprise: it is implemented by **moving the DMSD's 60 Hz horizontal
sync positions** — the clamp window is held *constant* (HC6B = $D4, HC6S = $CE across the whole
0…80 input range) while HS6B/HS6S move, sliding the fixed-width clamp window across the incoming
waveform and so changing the DC level the clamp establishes, i.e. the picture's black level [4].
The datasheet confirms every register's role and step size (HS6B/HS6S "horizontal sync begin/stop
for 60 Hz", step 2/LLC, two's complement with hidden sign bits; HC6B/HC6S the clamp pair) [2]
p. 23. Only the 60 Hz bank is touched — **50 Hz sources have no brightness control at all**
(§6.13) — and no compensating VDC XO write accompanies the shift, so the picture presumably
shifts horizontally with it (*inferred — unverified*).

The private selector block (335–357, beyond the published 1994 selector set) reaches the rest of
the VDC surface [4]: **352** toggles $00 bit 7 (RTB — an anti-gamma bypass switch, which is why
the gamma-value selectors were never needed), **342** writes the keyer limits $0C–$0F (the
chroma-key range setter), **335/336** write $10 (whose only runtime-significant field is MCT —
greyscale polarity), **350/351** set/get the "TV mode" flag, and 344/345 drive the 8-bit-screen
key-colour blend. Their hardware effects are known; their QuickTime-era names are not (§6.7).

### 4.8 The grab path

Per-grab arming, from the geometry/enable paths [4]:

```
driver Control cscSetVidInMode (csMode = 8 or 16)     ; PCBR bit 3, BusSize 0, clock divides
driver Control cscSetVidInRect (csRect)               ; VidInSize, VInH*/VInV*, may clip
VDC: $01..$08      ; XD/XS/XO horizontal, YD/YS/YO vertical — the scaling ratio
VDC: $09/$0A/$0B   ; VS/VC vertical bypass (VBI) region, or VC = 0 to disable it
VDC: $0C..$0F      ; keyer U/V limits (lower > upper = keyer off)
VDC: $00 |= $10    ; VPE := 1 — enable the VRAM output port ("capture enable")
CIVIC: VDCEnb := 1 ; VDCClk := 0                      ; arm the field interrupt, ungate the clock
async fields armed                                  ; base, bytes/line, row bytes, line count, done flag
   ... one VDC field interrupt per field ...
```

The VDC writes **continuously** into $50200800; the CPU copies out on the field interrupt. The
interrupt path, as recovered from the component's slot-0 element [4]:

1. **Service (slot-interrupt time):** read VDCInt; bit 0 set → not ours, decline. Otherwise
   acknowledge (VDCClr 0 then 1); if an asynchronous grab is pending, write **VDCClk = 1** to
   *freeze* the buffer — "to ensure that the data in VRAM isn't overwritten with a new field in
   the event that we take too long" — then defer the copy to a user-level function and claim the
   interrupt.
2. **Deferred copy:** count the field; if a grab is pending, re-assert the freeze, read CIVIC's
   VidInSize for the source stride (**1024 or 1536 bytes**), and move the field line by line from
   $50200800 to the client buffer — via a hand-rolled move routine that runs a 100-entry unrolled
   block of 68040 `MOVE16` instructions for 16-byte-aligned transfers and falls back to
   `_BlockMoveData` otherwise, sized for "768*2 = 1536 bytes max" — the PAL full-field width in
   16 bpp. If software line doubling is on, each source line is written **twice** to consecutive
   destination rows; then the client's done flag is set ($FFFF).
3. **Field filtering and callback:** if the application installed a user interrupt, the handler
   decides even/odd from the field counter (any-field requests alternate on its LSB) and calls
   the callback with a flags word (1 = even field, 2 = odd) and the application's refcon, all
   registers protected.

Because the buffer is frozen during the copy, an asynchronous grab is double-buffered by time
rather than by space: the capture hardware's continuous write is what makes the freeze necessary,
and the VOEN-accepts-VCLK clause of §3.7 is what makes the freeze safe. Line doubling, when the
1 MB VRAM budget forces it, is available twice over — CIVIC's hardware doubling bit
(VInDoubleLine, cleared at open) and the software copy's write-each-line-twice mode — and the
geometry path selects between them from the captured line count [4].

### 4.9 The System Enabler 088 build

System Enabler 088 ships a newer digitizer that supersedes the ROM's at first resource lookup [4]:

| | ROM build | Enabler 088 build |
|---|---|---|
| code resource | `'code'` −16729, 29452 B | `'vdig'` −16728, 29174 B, named "Built-in Digitizer" |
| component resource | `'thng'` −16727, 56 B long form | `'thng'` −16729, 44 B short form |
| I²C component | present in the ROM | **absent** — the Enabler's digitizer relies on the ROM's I²C component |
| register default tables | §2.3, §2.4 | **byte-for-byte identical** |
| subaddressed reads | served from the 42-byte shadow | **dead** — an unconditional branch skips the shadow lookup; every read goes to Cuda |
| buffer liveness probe | absent (a 60-tick field-arrival wait) | **present**: writes $0001FEFF to $50200804, starts the VDC, waits a field, and checks whether the pattern was overwritten, using the VidInSize stride |

The liveness probe is the strictest hardware contract in the whole programming model: a board
whose capture engine does not actually write into $50200800 when VDCClk = 0, BusSize = 0 and VPE
= 1 fails the Enabler's open. Why the Enabler simultaneously disables its own shadow read cache
is unknown (§6.15).

## 5. Quirks & errata

- **The developer note names the wrong Philips part.** "For details of VDC operation, see ... and
  the Phillips 7169 data sheet" [1] p. 15 — but SAA7169 is a 35 MHz triple 9-bit video D/A
  converter, a RAMDAC, which cannot be an I²C-controlled scaling datapath. The VDC is the
  **SAA7186**: its strap yields exactly $B8/$B9 [3] pp. 3, 19, it has exactly the 17 writable
  subaddresses the driver shadows [3] p. 19, it is specified against the SAA7191B source the board
  uses [3] p. 1, and its scaler/FIFO/VRAM-port feature set is the role the driver's programming
  implies.
- **Nothing is memory mapped.** Both control parts are behind Cuda's I²C master; no DecoderInfo
  slot, no part number, no Gestalt selector. A driver that goes looking for them in the I/O map
  finds nothing.
- **Both IICSA straps are tied low on purpose.** $8A and $B8 are the low-strap addresses of the two
  parts [2] pp. 21–22; [3] pp. 3, 19; strapping both low is what lets them share the one bus.
- **The write register file is write-only in practice.** Reads are served from the driver's
  42-byte shadow; only the two status bytes ever cross the bus [4]. Software that "reads back" a
  register is reading the driver's own memory.
- **The block write is not a block write.** The driver issues one one-byte I²C transaction per
  register even though both parts support subaddress auto-increment [3] p. 19; [2] p. 21 — the
  42-register boot sequence is 42 Cuda round trips.
- **The shipped overlay carries α = 0 everywhere.** The chroma keyer is off (lower limit +4 >
  upper 0 in both U and V), the documented idiom for "never inside" [3] p. 8 — so bit 15 of every
  captured 16-bpp pixel is 0, and the working overlay treats that as opaque video (§3.5).
- **The hardware cannot upscale.** XD ≤ XS and YD ≤ YS by register definition [3] p. 21; the
  capability flags advertise shrink/quarter/sixteenth and no stretch/double/quad [4]. A full-size
  capture is the ceiling, never a zoom.
- **The power-on window is even-fields-only.** OF = 11 discards the odd field by design [3] p. 15
  — half the vertical resolution for free — which is why the default geometry reads asymmetrically
  (640 in, 320 out, 1:1 vertically per field).
- **XO = 12 is out of the documented range.** The datasheet documents horizontal window start only
  from $010 [3] p. 21; the first geometry call overwrites it with in-range values, so nothing
  depends on it (§4.2).
- **Brightness is sync-position surgery, and PAL has none.** The control moves HS6B/HS6S in the
  60 Hz bank only [4]; a 50 Hz source never sees a brightness change (§4.7).
- **Contrast does not exist.** The selectors are dispatched and return "unimplemented" — the AV
  digitizer has no contrast control at all [4].
- **VDCInt is active low; VBL is active high.** The two CIVIC interrupt flags use opposite
  polarities, and the digitizer's handler tests bit 0 = 0 [4]. An implementation that copies the
  VBL convention breaks every grab.
- **The field interrupt shares the vertical-blanking line.** Slot 0, priority 200 under CIVIC's
  own 255 — and it is CIVIC's handler *declining* the interrupt that passes it to the digitizer
  [4] (§3.9).
- **Freezing the buffer does not disturb the capture engine.** VDCClk = 1 gates the VRAM writes
  while the FIFO still accepts VCLK pulses [3] p. 12 — the async grab's freeze/copy/unfreeze cycle
  depends on the letter of that clause.
- **The stride edge is 992 bytes, not a pixel count.** 496 pixels in 16 bpp, 992 bytes — the
  boundary between 1024- and 1536-byte rows [4]; the video driver and the digitizer each carry the
  constant, one as 496 pixels and one as 992 bytes.
- **The 660AV cannot auto-detect S-video.** Its input switch skips the probe/commit dance
  outright [4] — the one board-level behavioural difference between the two machines' identical
  digitizer silicon.
- **Convolution and video-in are mutually exclusive** [1] p. 410 — the flicker filter is lost for
  the whole session while a video window is active.
- **The colour killer is as insensitive as it goes.** CKTQ $FE truncates to the −30 dB extreme of
  the threshold range [2] pp. 23, 25 — and sets a reserved bit on the way; weak colour is never
  killed on this board.

## 6. Open questions

1. **What CIVIC latches as the VDC interrupt.** The VDC's once-per-field vertical-reset sequence
   on INCADR is the only candidate event the datasheet offers [3] p. 12; whether CIVIC wires the
   pin or derives the event from its own address generator wrapping, at what raster position the
   flag asserts, and whether a partially transferred field can raise it, are all unestablished
   (§3.9).
2. **How the capture buffer coexists with graphics in VRAM.** $50200800 is VRAM offset $100800,
   inside the second megabyte of the 2 MB aperture — yet the driver's size table has non-zero
   1 MB entries, and the video-capable mode tables use tiny graphics base offsets. Either the 1 MB
   decode aliases, or the 32-bit bus split carves a graphics half and a video half of the array;
   the exact partitioning is documented nowhere in the evidence set.
3. **The 8-bpp greyscale polarity paradox.** The VDC is programmed MCT = 1 (non-inverse
   monochrome) [4] while the video driver loads a **descending** 32-entry ramp for 8-bpp video-in
   [4] — composed naively those two yield a negative image. Either something inverts again inside
   Sebastian's 8-bpp video path, or the run-time flips MCT on one of the three paths that write
   $10 alone; which is unresolved.
4. **Whether the YUV compressed-capture path reprograms FS.** The scaler can emit YUV 4:2:2
   straight into VRAM (EFE = 0, FS = 01, matrix off) [3] p. 10, and the digitizer's compression
   group advertises a `'yuv2'` "Component Video - YUV" format [4]; whether the compression
   selectors reprogram FS or convert in software was not traced end to end.
5. **The exact QuickTime-constant-to-bit mappings for field preference and colour space.** The
   write sites and the datasheet encodings are both known (§2.4, §4.2); which OF pattern answers
   to which `vdFieldSelect` constant is inferred from the datasheet's odd/even labelling, not
   observed.
6. **The ≤ 8-bpp key-colour overlay path.** In 16 bpp the key is the α bit and the U/V limit
   registers; how the driver's 256-byte key-colour map, its saved graphics CLUT and the eight
   blend levels gate video against graphics inside Sebastian at 8-bpp graphics — where the
   monochrome output format carries no α bit at all [3] p. 10 — is untraced.
7. **The private selector block 335–357.** Only 335/336 (MCT), 342 (keyer limits), 350/351
   (TV mode), 352 (RTB) and 344/345 (key-colour blend) are identified, and those by hardware
   effect; roughly fourteen selectors, including two the component calls on itself, are
   unidentified, and the compression group 68–82 was not traced end to end.
8. **VSCDivide's semantics.** One bit, nominally the video-in shift-clock divide, but programmed
   as (8-bpp video-in AND 16-bpp graphics) — it depends on *both* depths, which a simple
   bytes-per-video-pixel reading does not explain.
9. **VInHFPD.** A two-bit "early H front-porch delay" register the driver programs with zero,
   always; its function is unknown.
10. **The analog front end.** The GPSW1 mux wiring, the S-video connector network, the clamp/AGC
    components and the LLC crystal trim are board-level and invisible to software; the DMSD's two
    out-of-datasheet-default H-sync-position values ($E5/$E9 vs $F4) are presumably that trim
    (*inferred — unverified*).
11. **The DMSD datasheet's GPSW1 pin contradiction.** The pinning table gives GPSW1 = pin 24 and
    GPSW2 = pin 25; the bit-function note says the reverse [2] pp. 3, 25. Nothing software-visible
    depends on it.
12. **How a DAV card "disables the DMSD".** The developer note says a DAV card may take over the
    VDC's input [1] p. 32, but not by what mechanism — an I²C write to the DMSD's output-enable
    bits, a hardware line, or the card simply driving the bus; no shipped software exercises the
    path.
13. **Brightness side effects.** Whether moving HS6B/HS6S shifts the picture horizontally (no
    compensating XO write is made), why the clamp window was repositioned once to HC6B = $D4 (the
    default table's $F4), and why the 50 Hz bank is left untouched are all open (§4.7).
14. **Selector numbering 36–40.** The two builds' dispatch chains step to different offsets by one
    entry across the clip-state/clip-region selectors; one of the two chain walks is off by one,
    and which is unproven.
15. **Why the Enabler build disables its own shadow read cache** with an unconditional branch over
    the shadow fast path [4] — a deliberate fix for a stale-shadow bug or an accident.

## References

1. Apple Computer, Inc., *Developer Note: Macintosh Quadra 840AV and Macintosh Centris 660AV
   Computers*, Developer Press, 1993 — §"Summary of Features" p. 5 (the widely compatible video
   input; RGB stored in a frame buffer separate from main memory); §"Cyclone Integrated Video
   Interfaces Controller" p. 14 (CIVIC "generates vertical blanking and video-in interrupt
   signals"); §"Video Data Path Chip" p. 15 (the VDC bullet list; the "Phillips 7169 data sheet"
   pointer); §"Sebastian" p. 15 (video/graphics mixing); §"Video and Graphics I/O" p. 31 (one or
   two VRAM frame buffers); §"External Video Input" p. 32 (Figure 2-8, the TDA8708/TDA8709 ADCs,
   the DMSD, CCIR 601-2, the DAV takeover, the VDC's scale/convert/store sentence); Table 2-12
   p. 33 (the 7-pin input connector, I²C clock/data on pins 5/7); §"Video RAM Usage" pp. 33–34
   (two 0.5 MB banks; the 640 × 480 / 512 × 384 maximum windows; the 18.43 / 22.12 MB/s data
   rates; Table 2-13 graphics/video depths); §"DAV Connector" p. 42 (KEL 8801-40-170L, unscaled
   4:2:2 access); §"Video Driver" p. 410 (first built-in video-in; convolution unavailable with
   a video input window).
2. Philips Semiconductors, *SAA7191B Digital Multistandard Colour Decoder, Square Pixel
   (DMSD-SQP)*, product specification, April 1993 — features and general description p. 1 (the
   26.8 MHz crystal; 780/944 × f_H square-pixel LLC rates; 768/640 active samples; 4:2:2);
   pinning p. 3 (IICSA pin 43; GPSW1/GPSW2 pins 24/25); §10 I²C-bus format and Table 5 status
   byte p. 21 (slave addresses $8A/$8B; STTC/HLCK/FIDT/CODE); Table 6 register map p. 22;
   function-of-the-bits pp. 23–26 (H-timing encoding, colour-killer range, BYPS/CHRS/GPSW,
   AUFD/OFTS, HRFS/VNOI, fixed-gain SECAM); programming example and Table 7 recommended defaults
   p. 31 (CHCV $2C/$59, the VCR-mode $80 note).
3. Philips Semiconductors, *SAA7186 Digital Video Scaler*, preliminary specification, May 1993 —
   features and general description p. 1 (4:2:2 required, SAA7191B source; 1023 pixel/line limits;
   line memories; 16-word FIFO; output formats); pinning p. 3 (IICSA pin 46, "LOW = B8, HIGH =
   BC"); functional description pp. 7–9 (decimation filters and adaptive mode AFS; the 768-pixel
   vertical-filter limit; RGB matrix equations; anti-gamma ROM γ = 1.4; the chrominance keyer and
   subaddresses 0C–0F; the two vertical regions and the videotext bypass; output levels 16/235,
   truncation and fill bytes); Tables 2–3 pp. 10–11 (the EFE/FS output formats; the 5-5-5 + α
   longword layout); output FIFO and VRAM output port pp. 12–13 (HFL/INCADR/VCLK/VOEN; the 32/16
   LLC burst rule; the VOEN-high FIFO-clocking clause; power-on reset state); field processing
   p. 15 (OEF detection, the nominal SAA7191B condition, POE, the OF INCADR/HFL sequences,
   register updates at the VS falling edge); §9 I²C-bus format and Table 4 register map p. 19;
   Table 5 status byte and Table 6 bit functions pp. 20–22 (ID3–ID0/OEF/SVP; the XD/XS/YO/AFS
   field semantics, the $010–$1FF XO range, MCT/QPL/QPP/TTR/EFE); §13 processing delays p. 28
   (58 LLC transparent-mode delay).
4. Macintosh Quadra 840AV / Centris 660AV boot ROM (2 MB mask ROM, release $10F3, version word
   $077D, mapped at $40800000) — full disassembly and resource-container decode. Cited sites,
   component-relative unless stated: the digitizer `'code'` −16729 at ROM offset $1A5200
   (maps at $409A5200, length $730C) and its `'thng'` −16727; the `'i2c '` `'code'` −16730 at
   $1AC5A0 with its shadow (25 + 17 bytes), 256-byte transfer chunking and read-serving rules;
   the open path $0C60–$1032 (the golden sequence of §4.2, registration gating, the 940-byte
   globals and the $2E-byte hold); the I²C helpers $55A0/$5630/$5750 (one-byte-per-transaction
   block write); the CIVIC primitives $57C0/$5810 (with the TV-mode interlock on VDCClk); the
   "TV mode" resynchronization at $00BA–$0102; the field-arrival probe $5BD0; the slot-0
   interrupt element (arm at $5A50, service at $6EA0: active-low VDCInt test, VDCClr 0-then-1
   acknowledge, VDCClk freeze, the deferred copy and the unrolled 100-entry MOVE16 mover sized
   for 1536-byte lines); the standard-selection path $3680; the input-selection path $33D0 (and
   the 660AV branch that skips it); the geometry engine $14FE with the VS/VC VBI programming,
   the field-line halving, the width fix-ups and the 992-byte stride edge; the destination-size
   group $26A8; the picture-control paths (brightness $1A22, sharpness $1C02 and its 16-entry
   aperture table); GetDigitizerInfo $1EC0; GetVideoDefaults $1DA8; the field-preference and
   colour-space write sites $25BE/$3E92; the private selectors 335–357 (352 → $00 bit 7,
   342 → $0C–$0F, 350/351, 344/345); the compression-group `'yuv2'` advertisement and its
   640 × 480 NTSC validation. Also from the same image: the video driver's CIVIC-side video-in
   code — the mode-set/rect-set control calls (csCode 136–139, csBaseAddr $50200800), the
   raster-geometry arithmetic and parking values of §4.5, the size table and its per-monitor
   maximum rectangles, the 32-entry video CLUT ramps (ascending 16 bpp, descending 8 bpp), and
   Sebastian's PCBR bit usage.
5. Apple Computer, Inc., System Enabler 088 (System 7.1 enabler for the Macintosh Quadra 840AV
   and Macintosh Centris 660AV, version 1.0, 1993) — resource inventory and disassembly. Cited:
   the `'vdig'` −16728 code resource (29174 bytes) and `'thng'` −16729 (44-byte short form,
   "Built-in Digitizer"); the byte-for-byte identical DMSD/VDC default tables; the dead
   shadow-read fast path (unconditional branch); the frame-buffer liveness probe writing
   $0001FEFF to $50200804 with the VidInSize stride; the absence of the `'i2c '` component (the
   ROM's is used); the STR/STR# name resources.
