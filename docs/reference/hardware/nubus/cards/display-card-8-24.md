# Apple Macintosh Display Card 8•24

**Contents:**

1. [Overview](#1-overview) — what the card is, the ASIC set, slot placement, the card family
2. [Register file](#2-register-file) — the slot-space decode; the JMFB, Stopwatch, CLUT and Endeavor blocks; reset state
3. [Behaviour](#3-behaviour) — monitor sensing, the mode catalog, frame-buffer addressing, timing, interrupts, palette and gamma, bus mastering
4. [Programming model](#4-programming-model) — the declaration ROM, PrimaryInit, SecondaryInit, the resident video driver, slot PRAM, multiple cards
5. [Quirks & errata](#5-quirks--errata)
6. [Open questions](#6-open-questions)

---

## 1. Overview

### 1.1 What the card is

The **Apple Macintosh Display Card 8•24** is Apple's 1990 NuBus colour display card for the
Macintosh II family: the product-line successor of the Macintosh II Video Card whose architecture
*Guide to the Macintosh Family Hardware* presents as the family example — timing generation
circuitry, a frame-buffer controller, dual-ported video RAM, a CLUT DAC and a declaration ROM
[1] §"Expansion Card Video" pp. 404–409. *Designing Cards and Drivers* names the three
then-current Apple display cards — the **4•8**, the **8•24** and the **8•24 GC** — and directs
developers to their Developer Notes, APDA publication M0857L/A [2] p. 245 [6]. The card carries
1-, 2-, 4- and 8-bit indexed pixel depths on every Apple monitor and a direct (24-bit colour)
mode, and identifies the connected monitor on three sense lines at startup, configuring itself
for it — the behaviour the whole card family inherited from the Macintosh II Video Card
[1] p. 404, [3] §"Monitor Support".

The card identifies itself, in its own declaration ROM [4], as follows:

| Field | Value | Where |
|---|---|---|
| Board sResource name | `Macintosh Display Card` (C string) | chip[$40D4] |
| Revision / part strings | `MDC 1.2` / `341-0868` | VendorInfo sub-list, chip[$428C] |
| Driver name | `.Display_Video_Apple_MDC` (Pascal string in the DRVR header) | chip[$72E5] |
| sBoardId | $27 (39) | inline, chip[$40B0] |
| sRsrcFlags | $0006 = fOpenAtStart + f32BitMode ([declaration-rom.md](../declaration-rom.md) §4.1) | inline, chip[$40B8] |
| Functional sRsrcType | category $0003 catDisplay, cType $0001 typVideo, DrSW $0001 Apple, **DrHW $0019** | chip[$4A34] |
| Format block | byteLanes $78 (byte lane 3), length $4000, CRC $D1629664 | chip[$7FEC] |

**A note on names.** The frame-buffer controller ASIC is designated **JMFB** in Apple's internal
designations of the era, and the card project appears under the name **Elmer**; the companion
timing/interrupt controller, colour DAC and pixel-clock synthesizer are designated **Stopwatch**,
**ACDC** and **Endeavor** respectively. None of these names appears in any Apple-published
document in the evidence corpus — they are recovered designations from the reverse-engineering
corpus around the card family, and this page uses them as the working names for the four register
blocks. The expansion of the acronym "JMFB" is not established by any source in the corpus (§6.1).
Everything asserted about the blocks below rests on the card's own ROM code and recorded guest
behaviour, not on the names.

### 1.2 The ASIC set and register blocks

The card is built around four custom parts, each mapped as a register block in a 1 KB window at
slot offset $200000. The window's existence, the block bases and every access sequence below are
observed in the card's own ROM code [4]; the family architecture (frame-buffer controller with
control registers "mapped into Macintosh II main memory in the slot space assigned to the video
card", the control address space "separate from the frame buffer address space", dual-ported
VRAM with the processor holding access "more than 95 percent of the time", and a single
CLUT-plus-three-DAC interface chip) is the documented video-card architecture of the family
[1] pp. 405–406. The 8•24 GC — whose display circuitry "closely matches the behavior of the
Macintosh Display Card 8•24" [3] — is described with the same blocks: an **MFB** frame-buffer
controller, an **AC842** custom chip with "three digital-to-analog converters and three
dual-port color tables capable of supporting 256 8-bit levels each", 2 MB of VRAM, and a
**programmable pixel clock** that the card programs at boot "to generate the signal that defines
the period for refreshing each individual pixel" [3] Figure 1 and part list.

| Block | Slot offset | Function |
|---|---|---|
| JMFB | $200000 | frame-buffer controller: control/status, video base, row stride (§2.2) |
| Stopwatch | $200100 | video timing and vertical-blank interrupt control (§2.3) |
| ACDC | $200200 | colour look-up table and RGB DACs (§2.4) |
| Endeavor | $200300 | programmable pixel-clock synthesizer (§2.5) |

The card carries its own VRAM in 512 KB and 1 MB configurations (§3.3) and a 32 KB declaration
ROM chip exposing an 8-bit bus footprint at the top of slot space (§4.1).

### 1.3 Slot placement and the bus view

The card works in any NuBus slot $9–$E; like every card it answers its 16 MB standard slot space
and is reachable in 24-bit mode through the first megabyte — minor slot space
([nubus.md](../nubus.md) §2.1, §2.3). It is a slave card: nothing in the evidence set shows it
mastering the bus, and its younger sibling the 8•24 GC needed a separate NuBus controller part
(the RDNC) to become a master [3] Figure 1. Its only asynchronous signal to the host is the
slot's /NMRQ line, driven at vertical blanking ([nubus.md](../nubus.md) §3.7; §3.5 below). The
power and connector environment is the standard NuBus card envelope ([nubus.md](../nubus.md)
§1.5–§1.6).

### 1.4 Family and variants

- The **Macintosh Display Card 4•8** is the monochrome sibling of the same design generation;
  the reverse-engineering corpus places it on the same frame-buffer controller and register
  model as this card (*inferred — unverified*; its declaration ROM, part 341-0801, is not in the
  corpus, §6.14).
- The **Macintosh Display Card 8•24 GC** (1990) keeps a display path that "closely matches the
  behavior of the Macintosh Display Card 8•24" [3], but is a different board: its declaration
  ROM is a 64 KB chip on byte lane 0 (byteLanes $E1) rather than a 32 KB chip on lane 3, its
  resident driver is named `.Display_Video_Apple_MDCGC`, and its display registers are driven
  from super-slot space rather than through the standard-space $200000 window of this card
  [5]. Two behavioural divergences are documented: the GC "does not fall back into a dormant
  state when it finds no monitor connected during boot time", explicitly unlike its companion
  8•24 [3]; and the GC is a NuBus master that uses block transfers, which this card is not
  known to be ([3]; §3.7).
- The **Macintosh II Video Card** is the predecessor whose board architecture the family
  documentation describes [1] pp. 404–409, [2] Chapter 11 pp. 245–260; its frame-buffer
  controller is the "FBC" (also called TFB) with control registers in slot space [1] p. 405,
  [2] p. 248 — the direct ancestor of the JMFB block of this card.

## 2. Register file

### 2.1 The slot-space decode

All card resources live in the card's 16 MB standard slot space, addressed in 32-bit mode as
$Fs000000 + offset (the driver swaps the MMU to 32-bit mode around every register access, and
restores it after [4]):

| Slot offset | Resource | Notes |
|---|---|---|
| $000000–$0FFFFF | VRAM, first megabyte | the minor-slot-space footprint; aliased across the slot (§3.3) |
| $200000–$2003FF | the four register blocks | §2.2–§2.5; all accesses longword-wide |
| $FE0000–$FFFFF8 | declaration-ROM bus footprint | 128 KB sparse footprint of the 32 KB chip, byte lane 3 (§4.1; [declaration-rom.md](../declaration-rom.md) §1.2) |

Registers are longword-spaced: the driver addresses them as 32-bit quantities with the
meaningful data in the low word (writes are zero-extended words; reads extract bitfields from
the longword) [4]. The declaration-ROM footprint is not a register resource and is addressed
byte-wise through the lane mask (§4.1).

### 2.2 The JMFB block ($200000)

| Register | Offset | Width | Function |
|---|---|---|---|
| Control/status (CSR) | $200000 | R/W longword | sense code (read), mode/state bits (§2.2.1) |
| Load/sync | $200004 | W longword | written once per mode programming; semantics not established |
| Video base | $200008 | W longword | frame-buffer origin within VRAM, in units of 32 bytes |
| Row words | $20000C | W longword | stride encoding (depth-dependent) |

#### 2.2.1 Control/status register

The known bit assignments, with the sense field read at the top:

| Bits | Name | Value | Meaning |
|---|---|---|---|
| 11–9 | sense | — | 3-bit monitor-sense code, read-only input (§3.1); the driver extracts it with a 68k bitfield instruction at MSB offset $14, i.e. conventional bits 11..9, and the same three bits are masked off with $F1FF for mode writes |
| 15 | reset | $8000 | master reset; set once in the mode sequence and never cleared by the driver's $F1FF mode mask |
| 13 | clock select | $2000 | pixel-clock source select; set before the clock-settle delay |
| 6 | video enable | $0040 | video-transfer-cycle enable; the final step of every mode sequence |
| 3 | refresh | $0008 | VRAM refresh enable; set in the mode sequence and present in the post-PrimaryInit read-back |

The mode writes in the card's own code always take the form `ANDI.W #$F1FF` (clear bits 14–9,
preserving bit 15 and bits 8–0) followed by `ORI.W #<bit>`, so bits 13, 6 and 3 are placed one
at a time as sequenced steps, and bit 15 is sticky once set [4] (*observed*; the per-bit names
are the register-model reading of the family, *inferred* for bits 6/13/3 beyond their observed
step roles). The driver's observed mode sequence is: set bit 13, wait for the clock to settle,
set bit 15, set bit 3, program the CLUT defaults, fill the frame buffer, set bit 6 (§4.2).
A live boot shows the CSR longword reading $00000000 before PrimaryInit and $0000000C after it
(*observed*, [4] run-time session) — bit 3 set, plus bit 2, whose meaning is not established
(§6.5).

#### 2.2.2 Video base register

Written with the frame-buffer origin as a byte offset from the VRAM base **divided by 32**:
the driver computes the mode's byte offset and shifts it right five places before the store
[4]. The mode catalog's `vpBaseOffset` values map to register values $2000→$100, $A00→$50 and
$80→$04 (§3.3).

#### 2.2.3 Row words register

Written with the mode's stride word from the per-depth parameter data. For the indexed depths
the frame-buffer row length in bytes is the register value times four (e.g. the 640-wide
8-bpp mode's 640-byte row is written as 160); for the direct mode the 640-wide entry is
written as 240 and the row is 2560 bytes [4] (*inferred* from the catalog's rowBytes values;
the exact hardware arithmetic for the direct mode is not established, §6.2).

### 2.3 The Stopwatch block ($200100)

| Register | Offset | Function |
|---|---|---|
| Interrupt/control | $20013C | VBL interrupt control |
| Interrupt clear | $200148 | write-1-to-clear pending vertical interrupt |
| Status | $2001C0 | vertical-blank phase bit (conventional bit 2 of the longword) |
| Interrupt status | $2001CC | interrupt-pending bit (bit 3 of the low word) |
| Timing image | $200100–$20011C | 15 longwords of per-mode video timing, plus two sentinel longwords |

Observed protocol [4]: the driver **enables** the VBL interrupt by writing $00000005 to
$20013C and **disables** it by writing $00000007 (Close does the latter); the vertical
interrupt is acknowledged by writing $00000001 to $200148; the card asserts the slot /NMRQ
at vertical blanking while enabled. The synchronous-code helper `WaitForVSync` polls the bit
at $2001C0 through a 1→0→1 transition to find the vertical blanking edge, after raising the
68k interrupt priority to at least 2 so the slot interrupt itself stays enabled; the VBL
handler spins on the pending bit at $2001CC until it clears, then acknowledges. The 15-word
timing image is written per mode from the mode's parameter data, followed by two sentinel
longwords — $00000006/$00000007 from PrimaryInit, $00000004/$00000005 from the driver's mode
path — whose role marks the end of the load; the field semantics of the 15 words are unknown
(§6.3).

### 2.4 The ACDC block ($200200)

| Register | Offset | Function |
|---|---|---|
| CLUT address | $200200 | palette index; a write resets the RGB sub-counter |
| CLUT data | $200204 | three sequential writes load R, G, B; the palette index auto-advances after the third |
| Pixel-bus control | $200208 | per-depth pixel-bus/depth code |

Observed protocol [4]: a palette load writes the start index to $200200, then streams
gamma-corrected RGB triplets to $200204 (§3.6); in the direct mode the driver writes a
clear-clear-value trio per entry instead. The pixel-bus control register receives the depth
code as the first word of each depth's parameter block (§4.2); the exact bit encoding — which
bits distinguish 1/2/4/8 bpp and which selects the direct mode — is not established (§6.2).
The driver enforces a per-depth palette-size limit from a five-byte table in the ROM
(chip[$7300] = `01 03 0F FF FF` — maximum CLUT index 1, 3, 15, 255, 255 for the five depths)
[4]. The DAC side — three 8-bit converters producing RS-343-A-compatible RGB video [1] p. 406,
and three dual-port 256×8 colour tables on the GC's closely-matching AC842 [3] — is not
separately register-programmed on this card beyond the CLUT protocol above.

### 2.5 The Endeavor block ($200300)

| Register | Offset | Function |
|---|---|---|
| PLL divisor M | $200300 | synthesizer divisor |
| PLL divisor N | $200304 | synthesizer divisor |
| External clock select | $200308 | clock source select |
| Reserved | $20030C | unused by the card's own code |

Observed protocol [4]: the initialization code writes $55 and then $AA to $200300 and reads
the register back to test that the clock block is answering; depending on the result it then
writes either a 3-byte or a 16-byte setup pattern taken from the mode's parameter data. The
family carries two pixel-clock generators — an Apple part and a National Semiconductor
alternate — distinguished at init by an ID read (*inferred*, register-model reading; the
identification values are not established for this card, §6.4). The programmable pixel clock
itself is documented family behaviour: the card "detects the monitor being used and programs
this chip to generate the signal that defines the period for refreshing each individual
pixel" [3] Figure 1 part list.

### 2.6 Reset state

Almost nothing about the power-on state of the register file is pinned by the evidence. Two
facts are: the CSR reads $00000000 at power-on and $0000000C once PrimaryInit has run
(*observed*, [4] run-time session), and the sense field reads the strapped monitor at all
times. Whether the VBL interrupt is masked or enabled at power-on, and the reset values of
the timing, CLUT and clock blocks, are unknown (§6.5).

## 3. Behaviour

### 3.1 Monitor sensing

The card reads a 3-bit monitor type from the sense lines at the CSR (bits 9–11, §2.2.1). The
base protocol is the family's: three sense lines on the DB-15 video connector (pins 4, 7 and
10, alongside RGB, composite/vertical/horizontal sync and grounds) [1] Table 12-1 p. 408,
read by the declaration ROM's initialization code at startup [1] p. 409, [2] pp. 254–255.
The published base table assigns 001 to the Portrait monitor, 011 to the Two-Page monitor,
110 to a 640×480 monitor and 111 to "no monitor connected", with the remaining codes reserved
[1] Table 12-2 p. 409.

This card uses all seven codes and extends the scheme. When the raw code reads 7 — the
"no monitor" code of the base scheme — the PrimaryInit code runs the **extended sense-line
probe**: the card pulls each sense line low in turn and reads back what the other two
return, assembling a composite code from the three read-backs [4] (subroutine at chip[$6FF0]).
The protocol and its rationale are documented for the 8•24 and 8•24 GC together: three sense
lines "limit the number of different monitors to seven", so "newer display cards use an
extension to the sense line scheme that allows for 28 new codes", with the encoding built
from "a few inexpensive diodes and a little wire" in the monitor or cable, and existing
monitors still detected correctly [3] §"Extended Sense Line Protocol" (with worked PAL
examples).

The observed code dispatch in the card's own PrimaryInit [4]:

| Code | Meaning |
|---|---|
| raw 7 | extended probe required; composite result decoded below |
| composite $14 | 640×480 RGB (13-inch class) |
| composite $2D | Portrait display |
| composite $00, $30 | no monitor |
| any other composite | "unknown / vendor-specific" marker in the code's internal monitor-type byte |

The raw (non-extended) codes select monitor families through a look-up table at chip[$70B2];
the raw-to-sRsrc mapping has been exercised end-to-end across cold boots and is:

| Raw code | Monitor family | Active 1 MB-configuration sRsrc (sister ID) | Grid |
|---|---|---|---|
| $0 | Two-Page / 21-inch Colour class | $A7 | 1152×870 |
| $1 | Macintosh Portrait Display | $A1 | 640×870 |
| $2 | 512×384 RGB ("small RGB") | $A2 | 512×384 |
| $6 | high-resolution 640×480 RGB (13-inch class) | $A6 | 640×480 |

(*observed*: cold boots of a Macintosh IIcx with the genuine card ROM at each strapped sense
code resolve exactly these sResources [4].) Codes $3–$5 are assigned in the card's table but
were not exercised in the corpus; PAL and NTSC output timings are reached only through extended
sense-line configurations [3] §"Monitor Support", and the card's mode catalog carries the
corresponding 512×384 / 640×480 NTSC and 768×576 / 640×480 PAL families (§3.2).

If no monitor is connected, the card does not come up as a screen: the 8•24 GC's designers
state that their card, unlike "its companion Macintosh Display Card 8•24", "does not fall
back into a dormant state when it finds no monitor connected during boot time" [3] — so the
8•24 does fall back into a dormant state (*inferred* from that explicit contrast; the exact
dormant state — which sResources are withdrawn, what PrimaryInit returns — is §6.6).

### 3.2 The mode catalog

The declaration ROM's sResource directory carries the board sResource plus 41 functional
sResources, one per (monitor × depth × configuration) combination, in four "sister" groups
[4]. The ID scheme, as decoded from the directory and the catalog walk [4]:

| ID bits | Meaning |
|---|---|
| low nibble | the monitor/mode family (table below) |
| bit 4 | the active flag — Apple's standard convention for the currently selected sister |
| bit 5 | the 1 MB-VRAM / 32-bit-addressed configuration (clear = 512 KB, 24-bit-addressed) |

PrimaryInit activates an $8x or $Ax sister (24-bit-addressed vs 32-bit-addressed pair); a
later init pass can upgrade to the $9x/$Bx sister (§4.3), exactly the standard video-card
convention of duplicated sResource lists for 24-bit and 32-bit environments [2] pp. 182–183,
Appendix B p. 557.

The monitor/mode families, from the decoded catalog [4]:

| Low nibble | Monitor (name string in the ROM) | Grid | dpi | vpBaseOffset | Depths (1 MB configuration) |
|---|---|---|---|---|---|
| $0 | 640×480 compatibility mode (stride 1024) | 640×480 | 72 | $2000 | 1, 2, 4, 8 |
| $1 | `Mac Portrait Display` | 640×870 | 80 | $80 | 1, 2, 4, 8 |
| $2 | 512×384 RGB ("small RGB") | 512×384 | 72 | $A00 | 1, 2, 4, 8 |
| $3 | `Mac Two-Page Display` | 1152×870 | 77 | $80 | 1, 2, 4, 8 |
| $4 | 512×384 NTSC-convolved mode (stride 1024) | 512×384 | 72 | $2000 | 1, 2, 4, 8 |
| $5 | `Mac 16″ Color Display` | 832×624 | 72 | $80 | 1, 2, 4, 8 |
| $6 | `Mac Hi-Res Display` (13-inch RGB) | 640×480 | 72 | $A00 | 1, 2, 4, 8, and 32-bit direct |
| $7 | `Mac 21″ Color Display` | 1152×870 | 77 | $80 | 1, 2, 4, 8 |
| $8 | 768×576 PAL (stride 768/1024) | 768×576 | 72 | $2000 | 1, 2, 4, 8 |
| $C | 640×480 NTSC (stride 1024) | 640×480 | 72 | $2000 | 1, 2, 4, 8, and 32-bit direct |

(The 16-inch and 21-inch names carry the ROM's smart-quote inch marks; the Two-Page and 21-inch
families share the 1152×870 grid with different timing data. A `Mac RGB Display` name string
is also present in the name table.) Each catalog entry records a page count — the number of
screen-sized pages that fit in the configuration's VRAM — and the counts confirm the two
configurations: the 512 KB sisters cap at e.g. 7 pages of Portrait 1-bpp (487 KB) while the
1 MB sisters reach 15 (1.04 MB) [4] (*observed* catalog values; the configuration sizing is
*inferred* from them).

The **direct (24-bit colour) mode** appears only as a fifth depth entry on the $Ax arms of the
NTSC-compatibility, small-RGB and high-resolution families ($A0/$A2/$A4/$A6/$AC), always with
one page and a 2560-byte row at 640/512 width [4]. Because those arms are the *inactive*
sisters of each pair (§4.5), the direct mode is reachable by the driver at runtime but is
never the persisted boot default — recorded boots confirm the boot-time mode picker cannot
select it (§4.5) (*observed*).

### 3.3 Frame-buffer addressing and VRAM apertures

The functional sResources declare the frame buffer in minor slot space: `sMinorBaseOS`
$00000000 with `sMinorLength` $00080000 on the 512 KB sisters [4]
([declaration-rom.md](../declaration-rom.md) §4.1) — the frame buffer is anchored at the
slot-space origin, in the first megabyte that 24-bit mode can address
([nubus.md](../nubus.md) §2.3).

The frame-buffer *origin within VRAM* is the mode's `vpBaseOffset` (§3.2), programmed into the
video-base register in 32-byte units (§2.2.2). Two aperture facts follow.

**First, the OS's screen pointer sits deep in slot space.** In a recorded boot of a Macintosh
IIcx in 24-bit Memory Manager mode with the Two-Page monitor family active, the boot ROM
resolves the screen base to **$F9900080** — slot base + $900000 + vpBaseOffset $80 — and
QuickDraw's inner loops store through that pointer directly, with no address stripping
(*observed*, [4] run-time session: ScrnBase at low-memory $0824 = $F9900080, ScreenRow $90,
screen bounds 1152×870, screen size $100000). Writes there must reach the frame buffer.

**Second, the same byte is reachable at two offsets.** In 24-bit mode the MMU translates an
address of the form $Fssx xxxx to $Fs0x xxxx ([nubus.md](../nubus.md) §2.3), so the pointer
$F9900080 dereferences to slot offset $000080 in 24-bit mode — while a 32-bit-mode program
holding the same pointer would reach slot offset $900080. Both land on the same frame-buffer
byte only if the card ignores the slot-space select bits above the first megabyte — i.e. if
VRAM is decoded on /AD19–/AD0 alone, which is exactly the card-design book's recommendation
for dual-mode addressing: "if you need less than 1 MB of address space... design your card to
use only bits /AD19–/AD0. By ignoring bits /AD23–/AD20, you guarantee that addresses of the
form $Fssx xxxx will be valid in both 24-bit and 32-bit modes" [2] p. 134.

The card does exactly that (*inferred — unverified* as to the exact decode width; the
observation that forces it is the $F9900080 screen base above). The consequence is that the
512 KB or 1 MB frame is mirrored at every megabyte boundary of the card's 16 MB standard slot
space, and software can address the visible frame buffer through any of the aliases — the
property the OS relies on when it hands QuickDraw a 24-bit-mode master pointer of the form
$F990xxxx. The driver's own 32-bit-mode pointer arithmetic uses the plain slot base plus
vpBaseOffset ($80, $A00 or $2000 per the mode, §4.3).

### 3.4 Video timing and the pixel clock

Per mode, the driver writes a 15-longword timing image to the Stopwatch block (§2.3) and a
pixel-clock setup to the Endeavor block (§2.5), then sequences the CSR through its
clock-settle/reset/refresh/enable steps (§2.2.1). The PrimaryInit code inserts two
delay loops on the low-memory tick counter between the clock-source step and the reset step —
the PLL needs settling time after a clock change [4]. Which pixel clock is programmed is a
property of the sensed monitor, per the documented family behaviour [3] (§2.5); the RS-170A
(NTSC-related) and PAL output families run interlaced timings, and for depths of 1 to 8 bits
a convolution filter is applied before each line is produced on those outputs [3]
§"Monitor Support" (documented for the GC's closely-matching display path). The field-level
contents of the 15-word timing image are not decoded (§6.3).

### 3.5 Interrupts and vertical blanking

The card's only interrupt is the vertical-blank interrupt on the slot's /NMRQ line
([nubus.md](../nubus.md) §3.7). The service path, in full [4]:

1. The driver's `Open` allocates a 16-byte slot-interrupt queue element, fills it
   (`sqType` = 6, the slot-interrupt queue type; `sqAddr` = the VBL handler;
   `sqData` = the address of the card's $200148 register), and installs it with
   `_SIntInstall`; it then writes $00000005 to $20013C to enable the interrupt
   (§2.3). This is the standard slot-interrupt installation
   ([declaration-rom.md](../declaration-rom.md) §10.3; [2] pp. 197–198).
2. At vertical blanking the card asserts /NMRQ; the OS level-2 slot dispatcher calls the
   handler with A1 = `sqData`.
3. The handler calls the OS vertical-retrace vector at low-memory $0DBC (`jVBLTask`), spins
   until the pending bit (bit 3 of the word at slot offset $2001CC) clears, acknowledges by
   writing $00000001 to $200148, calls `jVBLTask` again, then routes the VBL to the screen's
   gDevice chain through the vector at low-memory $0D28, passing the slot number recovered
   from the register address; it returns "serviced" [4].
4. `Close` (and the interrupt-disable control arm) removes the element with `_SIntRemove`
   and writes $00000007 to $20013C [4].

Synchronous driver code that must not tear the display (palette loads, mode changes) finds
the vertical-blanking edge with the polled status bit and masks interrupts below level 2 for
the duration (§2.3) [4].

### 3.6 Palette, gamma and the direct mode

A palette update (the driver's SetEntries arm) gamma-corrects each entry through the
driver's private gamma table, then — inside the VSync-guarded window — writes the start
index to the CLUT address register and streams R, G, B bytes to the data register; entries
whose table carries a negative start index are written index-by-index through the address
register instead [4]. The per-depth palette limits of §2.4 are enforced, and a table whose
entry count exceeds the depth's limit is rejected [4]. The gamma correction is applied by
the **driver, in software**, before the bytes reach the CLUT [4]: the card's ROM carries six
gamma-table blocks for this purpose — `Mac Gray Gamma`, `Page-White Gamma`, two untitled
curves, `Mac Std Gamma` (Apple's standard 1.8 curve) and `Mac RGB Gamma` — and the driver
selects per monitor (*observed*: boots with different strapped monitors show measurably
different pre-distorted CLUT output for the same nominal colours; the 21-inch-class
references carry a visible tint in the pre-CRT bytes, which the monitor's own response
curve cancels on real hardware). A `SetGamma` call validates the supplied table header
(a $19-byte header and a version field are checked, mismatched lengths or flags force a
re-allocation of the private copy), and a null table resets the private gamma so that
subsequent palette writes go out uncorrected [4] (*observed* byte behaviour; the header
field semantics are those of the standard gamma record [2] pp. 213–216).

The **direct mode** is the fifth depth (mode $84, §4.5): 24-bit colour with a 4-byte-per-pixel
storage layout — the catalog's 2560-byte rows at 640-wide are four bytes per pixel, not three
(*inferred* from the catalog rowBytes; the byte order within the stored pixel is not
established, §6.2). On a direct device the regular SetEntries arm refuses with an error, and
the separate DirectSetEntries arm performs the indexed-style load through the
clear-clear-value trio (§2.4); the GrayScreen and gamma arms bear the responsibility of
setting the hardware up on direct devices [2] pp. 205–211 (the contract), [4] (the card's
implementation).

### 3.7 Bus mastering and block transfers

Nothing in the card's declaration ROM or driver mastering the NuBus is in the evidence set,
and the card has no bus-mastering companion part: the GC needed the RDNC NuBus controller to
"allow block transfers both as master and slave card" [3] Figure 1, and the GC's developers
describe the block-transfer acceleration (master and slave, with a "pseudo block transfer"
fallback for cards that do not support it) as a property of the GC generation [3]. Whether
this card's declaration ROM carries the block-transfer sResource entries at all — and
whether it accepts slave block transfers like its successor — has not been established
(§6.13).

## 4. Programming model

### 4.1 The declaration ROM

The card's declaration ROM is a 32 KB chip whose **used data is the last 16 KB** (the format
block's `Length` field reads $4000); the first 16 KB of the chip is zero padding [4]. The
format block sits at the top of the chip, chip[$7FEC..$7FFF], and reads [4]:

| Field | Value |
|---|---|
| Length | $00004000 (16 384 bytes) |
| CRC | $D1629664 |
| RevisionLevel | $03 |
| Format | $01 (the Apple format) |
| TestPattern | $5A932BC7 |
| ByteLanes | $78 — byte lane 3, complement valid |

The format block and its validation are the standard ones
([declaration-rom.md](../declaration-rom.md) §2); the lane-3 mask means the chip's 32 KB
occupies a sparse 128 KB bus footprint at the top of the card's slot space, chip byte *i*
appearing at slot offset $FE0000 + 4·*i* + 3 [4] ([declaration-rom.md](../declaration-rom.md)
§1.2). The Slot Manager's scan reads the card through exactly that footprint: recorded boots
show the boot ROM validating the format block, computing a matching CRC, and walking the
directory (*observed*; the board sResource's sub-entries are read hundreds of times during the
scan, the sPrimaryInit pointer dereferenced once) [4].

The chip layout [4]:

| Chip offset | Contents |
|---|---|
| $4000 | top-level sResource directory: board sResource (ID 1) + 41 functional sResources + terminator |
| $40A8 | board sResource sub-list: sRsrcType, sRsrcName `Macintosh Display Card`, sBoardId $27, sPrimaryInit (→ sExec block at $6BF6), sBoardFlags $1C4, sRsrcVidNames, revision and video-attribute sub-entries |
| $4190 | video mode name strings (10 entries, §3.2) |
| $4280 | VendorInfo sub-list: ID 1 `Apple`, ID 3 `1.2`, ID 4 `341-0868` ([declaration-rom.md](../declaration-rom.md) §5.3) |
| $42A8 | the 41 functional sResource sub-lists (sRsrcType with DrHW $0019, sRsrcName `Display_Video_Apple_MDC`, sRsrcDrvrDir, sRsrcFlags $0006, sRsrcHWDevId 1, sMinorBaseOS/sMinorLength, five spDepth entries) |
| $4A90 | six gamma-table blocks (§3.6) |
| $5D00 | the sParms records: per-(monitor, depth) parameter and VPBlock data |
| $6BE0 | the PrimaryInit sExec block (§4.2) |
| $72CE | the driver block (§4.4): sBlock prologue $00000D16 + $4C000000, DRVR header, code |
| $7FEC | format block |

### 4.2 Boot: PrimaryInit

PrimaryInit is the sExec block the Slot Manager copies to low RAM and executes during the
per-slot scan ([declaration-rom.md](../declaration-rom.md) §8; the environment restrictions —
Slot Manager calls only, small code, quick exit, `seStatus` result — are the standard ones
[2] p. 176). The card's block, annotated from its own code [4], performs, in order:

1. Builds the card's 32-bit slot base from the slot number in the seBlock, allocates a
   56-byte parameter block, and calls `_SlotManager` selector $08 (`sFindStruct`) on its own
   board sResource to detect whether the new Slot Manager is present — a failure latches a
   flag that later selects old-Slot-Manager behaviour.
2. Switches the MMU to 32-bit mode and reads the 3-bit sense field from the CSR (§3.1);
   raw code 7 triggers the extended sense-line probe.
3. Reads the slot's parameter record (selector $11) — the saved mode — and compares the
   saved monitor type with the sensed one; a mismatch sets a "changed" flag that will make
   it rewrite the record.
4. Probes the card's presence on the bus with the $55/$AA read-back test at the clock block
   (§2.5) and records the result in the parameter block.
5. Walks the sResource lists (sRsrcType / sNextSRsrc / sReadInfo) to fetch the sParms record
   for the chosen depth of the sensed monitor's sResource.
6. Programs the hardware from the sParms data: the depth code word to the pixel-bus register
   (+$200208), four words — mode/control, load/sync, video base, row words — to the JMFB
   block, the 3- or 16-byte clock pattern to the Endeavor block, the CSR sequence
   (bit 13, settle delay, bit 15, bit 3: §2.2.1), the 15-word timing image plus sentinels to
   the Stopwatch block, and a six-byte default palette to the CLUT data register (after
   clearing the CLUT address register), taken from an eight-entry per-depth default table at
   chip[$70C6] and written in the interleaved byte order 1, 3, 5, 0, 2, 4 (§5).
7. Clears the mode's frame-buffer region and paints the boot gray: alternating rows of
   $AAAAAAAA and $55555555 across the mode's bounds — the 50-percent dithered gray the
   standard requires a card to paint before the first real screen [2] Appendix B p. 555.
8. Sets the final CSR step (bit 6, video enable), restores the MMU mode, releases the
   parameter block, and returns.

The activation dance around steps 5–6 (sUpdateSRT / sNextTypeSRsrc calls with the chosen ID,
setting and clearing the active bit, deactivating the alternate sister) is the standard
slot-record-table management of [2] pp. 176–179; the recorded boots show the whole sequence
completing and the CSR transitioning $00000000 → $0000000C (*observed*) [4].

### 4.3 SecondaryInit

The card carries a second sExec block executed after the system patches are installed
([declaration-rom.md](../declaration-rom.md) §9; [2] p. 179). Its code [4]:

1. Probes for 32-bit QuickDraw with `_GetTrapAddress` pairs — if the 32-bit QuickDraw traps
   resolve to the unimplemented trap, the card cannot upgrade its sResource selection.
2. Re-runs the slot-record activation for the upgraded (1 MB, 32-bit-addressed) sister ID,
   deactivating the depth-0 alternate per the standard sample's sequence [2] Appendix B
   pp. 557–560.
3. If the card is the boot screen, walks the OS device chain — MainDevice handle at
   low-memory $08A8 → gDevice → `gdPMap` → PixMap — and writes the frame-buffer pointer into
   the PixMap's `baseAddr` field, adding the mode's frame-buffer offset ($2000, $80 or $A00
   per depth class, §3.2) to the 32-bit slot base; this is the standard SecondaryInit
   gDevice patch [2] Appendix B pp. 560–561.
4. Walks the driver unit table to the card's DCE and updates its `dCtlDevBase` with the same
   pointer, so subsequent parameter-block calls carry the right base.

### 4.4 The resident video driver

The driver lives in an sBlock at chip[$72CE], size $0D16 (3350 bytes) with a $4C000000 flag
longword ahead of the DRVR header [4]; the Slot Manager loads it through the sRsrcDrvrDir
([declaration-rom.md](../declaration-rom.md) §7). The header is unusual: eight words —
flags $0000, delay $0000, event mask $0048, menu $0000, then the Open/Prime/Control/Status
offsets ($022A, $068C, $0658, $182E) — with the Pascal name `.Display_Video_Apple_MDC`
immediately after and **no separate drvrClose offset**: the Close routine is reached from
the dispatch code, not through the standard ninth header word [4] (*observed*; a strict
nine-word header reader mis-parses this driver, §5).

**`Open`** [4] allocates a 36-byte private storage record (installed in the DCE's
`dCtlStorage`), and:

| Private offset | Contents |
|---|---|
| $04 | cached frame-buffer pointer (`pmBaseAddr`) |
| $08 | the 16-byte slot-interrupt queue element |
| $0C | the color/gamma cache |
| $10 | capability flags — decoded from the DCE's `dCtlExtDev` byte: bits 0–2 depth class, bit 3 "big-screen", bit 4 32-bit-mode capable, bit 5 direct-mode capable; internal bits track wide depths, direct mode active, the 24-bpp variant, and bus presence |
| $12 | pointer to the active mode's sParms record |
| $16 | the last-seen `csMode` |
| $18 | the card's 32-bit slot base |
| $1C | depth class word |
| $1E | extended feature flags (the 24-bpp variant, the extended parameter paths) |
| $20 | video-name / fallback CLUT pointer |

It probes the bus, decodes the capability byte, installs the slot interrupt (§3.5), walks the
sResource chain for the active sParms and the matching gamma table (building a synthetic
fallback if none matches), and returns `noErr`; any Slot Manager failure releases everything
in reverse and returns an install error.

**Control arms.** The driver implements the standard video-driver control set [2] pp. 205–211;
each arm's behaviour as observed in its code [4]:

| Arm | Behaviour |
|---|---|
| SetMode | validates `csMode` ($80–$84 → depth index 0–4; index 4 needs the direct capability, the 32-bit flags, and sets the "direct active" latch), validates the page against the mode's page count, then runs the full programming sequence — timing image, clock pattern, CLUT defaults, CSR steps (§4.2) — recomputes the frame-buffer base (page × rowBytes × height + mode offset), clears the affected VRAM region, and returns the base in the parameter block |
| SetEntries | the palette load of §3.6; refused when the direct latch is set |
| SetGamma | validates and installs a private gamma copy (§3.6); a direct device additionally refreshes the CLUT from the new gamma |
| ResetEntries | re-programs CSR/CLUT/timings from the *current* sParms with no mode change, and regrays the screen |
| GrayScreen / SetGray | latch bits in the private flags that make subsequent palette loads write luminance-mapped gray instead of colour ([2] p. 205-level contract) |
| SetInterrupt | installs or removes the slot interrupt (§3.5) per the passed flag |
| DirectSetEntries | the direct-mode palette arm (§2.4, §3.6); refused unless the direct latch is set |
| SetDefaultMode / slot-PRAM write | writes the passed mode byte into the slot's parameter record through `_SlotManager` selectors $11/$12 — the persistence path of [2] p. 210 |

**Status arms** return the current mode and parameters per the standard status contract
[2] p. 211; one arm is pinned in the disassembly: the slot-PRAM byte read (selector $11,
returning byte 3 of the record) [4]. Every arm ends through the driver's completion tail,
which returns directly for synchronous calls and jumps through the low-memory `jIODone`
vector otherwise [4].

**`Close`** frees the private storage in reverse order, removes the slot interrupt, and
disables the VBL interrupt source (§3.5) [4] — the standard requirement that a video driver
be closable and reopenable at any time [2] p. 200-level note.

### 4.5 Slot PRAM and mode persistence

The slot's 8-byte PRAM record carries the board ID in bytes 0–1 and the card's state in the
rest ([declaration-rom.md](../declaration-rom.md) §5.2); on this card byte 2 is the saved
depth/mode (`savedMode`, $80–$84 for depths 1/2/4/8/direct), byte 3 the saved sResource ID,
and bytes 6–7 the sensed monitor's "sister" ID, written back by PrimaryInit on every boot
(*observed*: the recorded boots seed and read back exactly these fields, with BoardID $0027
matching the sBoardId of §1.1) [4]. The driver's SetDefaultMode arm is what the Monitors
control panel drives to persist a user choice [2] p. 210.

One asymmetry is load-bearing: the **direct mode is runtime-only**. Its sResource entries sit
on the inactive $Ax sisters (§3.2), and the boot-time default-mode picker — which only
resolves the active lists — cannot select them; recorded attempts to boot straight into the
direct mode are rejected during the boot ROM's sResource read, while the running driver
accepts `csMode` $84 and switches into it at runtime (*observed*) [4].

### 4.6 Multiple cards and the boot screen

The card is one of possibly several screens: the family software supports multiple cards as
an extended desktop [1] p. 404 developer tip. Recorded behaviour on a Macintosh IIcx with a
second video card in a higher slot: with virgin PRAM the ROM's boot-screen selection lands on
the **highest video slot**, painting the gray desktop dither into that card's frame buffer,
while every card's driver still runs and each secondary screen is grayed as well
(*observed*; the same rule holds with different card types in the higher slot) [4]. This is
the classic 68k NuBus rule and contrasts with the first Power Macintosh generation, where the
built-in monitor, not slot priority, decides ([bart.md](../../../machines/pdm/bart.md) §4.7).

## 5. Quirks & errata

- **The screen pointer sits nine megabytes into slot space.** The boot ROM hands QuickDraw a
  screen base of the form $F990xxxx (observed: $F9900080), far above the declared minor-slot
  footprint; only the card's megabyte-mirrored VRAM decode makes that pointer live (§3.3).
  A card model that maps VRAM at the slot origin only drops every QuickDraw store.
- **The frame buffer is mirrored at every megabyte boundary.** The same byte is reachable at
  slot offsets $000080 and $900080; 24-bit and 32-bit mode dereference the same OS pointer
  to the same pixel (§3.3). The exact decode width is inferred, not documented.
- **The DRVR header has eight words, not nine.** No `drvrClose` offset follows the Status
  offset; Close is reached through the dispatch code. A parser that assumes the standard
  nine-word header reads the driver name as the Close offset (§4.4).
- **The six-byte default palette is written in interleaved order.** PrimaryInit's boot
  palette load writes the six default bytes to the CLUT data register in the order 1, 3, 5,
  0, 2, 4 — not 0–5 (§4.2). The reason is not established.
- **The timing-image sentinels differ between code paths.** PrimaryInit terminates the
  15-word timing load with $00000006/$00000007; the driver's mode path uses
  $00000004/$00000005 (§2.3).
- **The VBL-interrupt control values are $00000005 (enable) and $00000007 (disable)**, not a
  single mask bit written as 0/1 (§2.3). Close always disables; Open always re-enables.
- **`WaitForVSync` raises the interrupt priority to at least 2** before polling the blanking
  phase, so the slot interrupt stays deliverable while display-critical sequences run (§2.3).
- **Gamma is a driver function, not a RAMDAC function.** The palette bytes are pre-distorted
  in software from the active gamma table before they reach the CLUT data register; the
  pre-CRT bytes therefore differ per monitor for the same nominal colour (§3.6).
- **The direct mode stores four bytes per pixel** — 2560-byte rows at 640-wide in a catalog
  that also claims 1 MB configurations and one page (§3.2, §6.8) — and is runtime-only: no
  cold boot can persist it (§4.5).
- **The extended sense probe reads back its own driven lines.** The composite code is
  assembled from what the *other* two lines return while each line is pulled low in turn
  (§3.1); a sense-code model without the read-back dependency cannot produce the PAL/NTSC
  configurations at all.
- **The driver's mode validation rejects depths by capability, not by catalog.** `csMode`
  $84 is accepted only when the DCE capability byte advertises direct mode and the 32-bit
  flags are set — the catalog alone does not decide (§4.4).
- **The card runs its init code relocated.** PrimaryInit and the driver execute from
  RAM copies made by the Slot Manager; the sExec block's header layout is not fully
  decoded, and the executed entry lands partway into the block rather than at its first
  code byte (§4.2, §6.7).
- **Lane 3, not lane 0.** The declaration ROM is byteLanes $78 — the 8-bit chip rides the
  *top* byte lane, so its bus footprint is the top 128 KB of slot space with chip byte 0 at
  slot offset $FE0003. The later 8•24 GC ROM is the opposite ($E1, lane 0) (§4.1, §1.4).

## 6. Open questions

1. **The JMFB designation.** No Apple-published document in the corpus names the ASIC; the
   expansion of the acronym and the silicon's relationship to the Macintosh II Video Card's
   FBC/TFB part are not established.
2. **The pixel-bus control encoding.** Which bits of $200208 select 1/2/4/8 bpp, which value
   selects the direct mode, and how the direct-mode row-words value (240) becomes a 2560-byte
   row are all unresolved; only the write traffic is observed (§2.4).
3. **The 15-word timing image.** The per-mode longwords written to $200100–$20011C and their
   sentinel pairs are observed writes whose field semantics (horizontal/vertical counts,
   sync widths, blanking) are not decoded anywhere in the corpus (§2.3). The canonical
   source would be the card's Developer Note [6].
4. **The clock-generator identity.** Which read-back result selects the 3-byte versus the
   16-byte setup pattern, and what ID values the two clock parts return on this card, are
   unknown; the two-part family and the $55/$AA probe are the only pinned facts (§2.5).
5. **Power-on state.** The reset values of every register except the CSR (which reads $00
   cold and $0C after init) are unknown, including whether the VBL interrupt sources at
   power-on; bit 2 of the CSR's post-init value $0C is unattributed (§2.6).
6. **The no-monitor dormant state.** That the card goes dormant with no monitor is inferred
   from the GC article's explicit contrast; which sResources are withdrawn, what PrimaryInit
   writes, and what the Slot Manager records for a dormant card are unobserved (§3.1).
7. **The sExec block header.** The 36-byte header ahead of the PrimaryInit code is only
   partly interpreted (size/revision/processor/entry-offset fields are guessed from the
   bytes), and the observed entry point lands inside the block; the exact field layout is
   unresolved (§4.2).
8. **The direct mode's VRAM demand.** The catalog's 2560-byte rows at 640×480 exceed the 1 MB
   configuration the sister grouping implies — either the direct-capable configurations carry
   more VRAM, or the direct mode uses a layout the catalog's rowBytes does not describe
   linearly; the observed sMinorLength ($80000, on the 512 KB sisters) is the only hard
   size datum (§3.2, §3.3).
9. **The stored pixel layout in the direct mode.** Four bytes per pixel is inferred from the
   row lengths; the byte order (RGBX, XRGB, BGRX) is not established (§3.6).
10. **The raw sense codes $3–$5.** Only raw codes $0, $1, $2, $6 and the extended protocol
    have been exercised; the remaining entries of the card's mode-id look-up table at
    chip[$70B2] are unverified, as is the low-nibble $9/$A/$B/$D/$E gap in the directory
    (§3.1, §3.2).
11. **The name-string-to-family mapping.** The board's name directory maps tags $B3–$BC to
    ten strings, but the assignment of the `Mac RGB Display` and the two PAL strings to
    specific catalog families is ambiguous in the ROM's own data (§3.2).
12. **Refresh rates and the VBL period.** No per-monitor refresh frequency is established;
    only the vertical-blank *phase bit* is observed. The timing image of item 3 is the
    blocker.
13. **Block-transfer capability.** Whether the card's declaration ROM carries the
    block-transfer sResource entries and whether the card accepts slave block transfers
    (as the GC does) is undetermined (§3.7).
14. **The 4•8 sibling.** Part 341-0801 is not in the corpus; the shared-register-model claim
    rests on the family's driver lineage, not on the card's own ROM (§1.4).
15. **The Rev A ROM and the version genealogy.** Only the Rev B ROM (part 341-0868,
    `MDC 1.2`, CRC $D1629664) is held; earlier revisions of the card's declaration ROM are
    not in the corpus, and the card's Developer Note [6] — the canonical register and timing
    reference — is not available to this evidence set.

## References

1. Apple Computer, Inc., *Guide to the Macintosh Family Hardware*, second edition,
   Addison-Wesley Publishing Company, 1990 — Chapter 12 "Displays" pp. 397–425; §"Expansion
   Card Video" pp. 404–409: the video-card component set (timing generation, Frame Buffer
   Controller, dual-ported video RAM with the processor holding access more than 95 percent
   of the time, CLUT DAC, declaration ROM) pp. 404–406; the DB-15 video connector and Table
   12-1 signal assignments p. 408; Table 12-2 sense line values p. 409; the monitor-sensing
   initialization flow p. 409.
2. Apple Computer, Inc., *Designing Cards and Drivers for the Macintosh Family*, third
   edition, Addison-Wesley Publishing Company, 1992 — Chapter 8 "NuBus Card Firmware":
   PrimaryInit restrictions and the SExecBlock environment p. 176, SecondaryInit p. 179,
   video-card firmware requirements and 24-bit/32-bit sResource list duplication pp. 182–183,
   sGammaDir / sRsrcVidNames / gamma table data pp. 185–186; Chapter 9 "NuBus Card Driver
   Design": slot device interrupts and SIntInstall pp. 197–198, video drivers p. 200, the
   video-driver Control and Status routine contract (csCode set) pp. 205–211, gamma
   correction pp. 213–216; Chapter 7 §"NuBus Address Space" pp. 132–134 (the 24-bit
   translation table and the /AD19–/AD0 decode recommendation p. 134); Chapter 11 "The
   Macintosh II Video Card" pp. 245–260 (FBC control registers p. 248, monitor sense-line
   selection pp. 254–255, connector pinouts pp. 258–259, the note naming the Display Card
   Developer Notes p. 245); Appendix B "Sample Video Card Firmware" pp. 539–561 (the gray
   screen p. 555, the 24-bit/32-bit sister lists p. 557, the SecondaryInit gDevice patch
   pp. 560–561).
3. Guillermo Ortiz, "Macintosh Display Card 8•24 GC: The Naked Truth," *develop*, issue 3,
   Apple Computer, Inc., July 1990 — the display circuitry that "closely matches the
   behavior of the Macintosh Display Card 8•24" and Figure 1's part list (MFB frame-buffer
   controller, AC842 with three DACs and three dual-port 256×8 colour tables, 2 MB VRAM,
   programmable pixel clock, RDNC NuBus controller); §"Monitor Support" (depth support,
   RS-170A/PAL outputs with interlacing and the convolution filter, the no-monitor dormancy
   contrast, block-transfer master/slave behaviour); the "Extended Sense Line Protocol"
   sidebar (the seven-code limit, the 28-code extension, worked PAL examples).
4. Apple Macintosh Display Card 8•24 declaration ROM, part 341-0868 Rev B (revision string
   `MDC 1.2`), 32 KB chip, byteLanes $78, format-block CRC $D1629664 — annotated disassembly
   of the whole chip and the recorded boot sessions behind it: the sResource directory and
   mode catalog (§3.2), the board sResource (chip[$40A8]), the VendorInfo sub-list
   (chip[$4280]), the gamma blocks (chip[$4A90..$502B]), the PrimaryInit sExec block
   (chip[$6BE0], code at chip[$6C08..$70F0], extended-sense probe at chip[$6FF0], default
   palette table at chip[$70C6]), the SecondaryInit block (chip[$7118..$72CC]), the driver
   block (chip[$72CE..$7FE3]: Open at chip[$731A], interrupt install at chip[$789C], the
   control arms, `ValidateMode` at chip[$7B08], `WaitForVSync` at chip[$7BBA], the timing
   path at chip[$7C00], `SetVideoBase` at chip[$7D32], Close at chip[$792A], the VBL
   handler at chip[$7FA0]); the run-time observations of the IIcx slot-manager walk, the
   $F9900080 screen base, and the CSR power-on values.
5. Apple Macintosh Display Card 8•24 GC declaration ROM, version 1.1 (part 341-0266, dated
   16-Sep-91), 64 KB chip, byteLanes $E1, format-block CRC $D722B053 — annotated
   disassembly: the GC's resident driver `.Display_Video_Apple_MDCGC` and its display
   registers driven from super-slot space (the family contrast of §1.4), and the sibling
   version genealogy (v1.0 part 341-0812-02, CRC $9E9857E8; alpha build 1.0A16, CRC
   $4740028D).
6. Apple Computer, Inc., *Display Card Developer Notes for the Macintosh Display Cards
   4•8, 8•24, and 8•24 GC*, APDA publication M0857L/A — the canonical register-level and
   timing reference for this card, named by [2] p. 245; not present in the evidence corpus
   and therefore not used as a source for any claim on this page (§6.3, §6.15).
