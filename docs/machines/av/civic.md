# CIVIC + Sebastian — the AV video path

CIVIC (343S1096) is the frame-buffer and video-timing controller; Sebastian
(343S0704) is the RAMDAC/CLUT downstream of it; an Endeavor (840AV) or
Clifton/PUMA (660AV) synthesizer supplies the pixel clock. All three live in
[src/machines/av/civic.c](../../../src/machines/av/civic.c) /
[civic.h](../../../src/machines/av/civic.h). Hardware reference:
`local/gs-docs/840av_660av/docs/civic.md`, `sebastian.md`,
`endeavor-clifton-puma.md`.

## The register interface is bit-serial

The thing to get right before anything else: **CIVIC's registers are one bit
per longword.** Only D[0] is meaningful, the LSB sits at the lowest address,
and the stride is 4 — so a 12-bit register at `$380` occupies `$380`…`$3AC`.
The driver streams writes LSB→MSB ascending and walks reads MSB→LSB descending
from `base + width*4`. Five 1-bit registers (VBLInt, VBLClr, Reset, SyncClr,
BusSize) are *also* poked as plain longwords.

The model is therefore one stored bit per longword slot, which serves both
access styles with no special-casing. Registers wider than a bit are simply
runs of slots; `civic_get` assembles one when the implementation needs its
value (BaseAddr, for the scanout pointer).

Computed slots, where a plain latch would be wrong:

| Slot | Behavior |
|---|---|
| VBLInt `$000` | live VBL flag, **active high** |
| VDCInt `$008` | constant 1 — **active low**, so 1 means "no video-in interrupt"; this is what makes the enabler's `vdig` fail to open cleanly |
| SyncClr `$06C` | **reads inverted** (the driver's open treats bit 0 == 0 as "driving composite") |
| ReadSense `$080` | the three sense lines from the monitor model |
| CntTest `$140` | reads are a pure settle delay — **side-effect-free**, returns 0 |
| CurLine `$6C0` | 0 |

## Monitor sense

A static Hi-Res 640×480 monitor (indexed code 6) is attached. A sense line
reads low when the monitor ties it low or the host drives it (`SenseN` = 1),
which reproduces the documented drive patterns — `CivicResetSenseLines`,
`CivicDriveA/B/C` — and yields the indexed code directly from an idle read.
The same static answer serves the ROM's extended tie-matrix probe.

## VBL

A 60.15 Hz scheduler event. When the timing generator (Enable) and VBLEnb are
on and the interrupt is armed, it latches VBLInt **and** asserts PSC-VIA2 SInt
bit 6 (active low). The second half is not optional: without it the level-2
handler never runs, so no VBL tasks fire and the cursor never blinks. The ack
is the driver's `VBLClr` 0-then-1 dance — 0 clears and disarms, 1 re-arms.

## Sebastian

Byte registers on a `$10` stride at `$50F30800`: index (`$000`), data
(`$010`), PCBR (`$020`). CLUT access is an index write followed by **four**
data bytes — R, G, B, alpha — with the address auto-incrementing after the
fourth. Two CLUT banks (graphics and video-in) are selected by PCBR bit 6; the
alpha byte is real state the driver read-modify-writes to preserve, so the
store is 256 × RGBA per bank.

PCBR bits 2–0 are the depth code (0–5 = 1/2/4/8/16/32 bpp) and drive the
display descriptor's format and stride. Sub-8bpp modes do not pack their
entries at the bottom of the bank: they live at `start + i*skip` where
`skip = 256 / 2^depth` and `start = skip − 1` (so 1 bpp uses `$7F` for white
and `$FF` for black), and the derived display CLUT gathers them from there.

## Clock synthesizer

Pure write latches at `$50F2E000`. The frequency formulas are undocumented
even in Apple's source — the per-mode M/N (Endeavor) and W (Clifton/PUMA)
values are opaque signatures — and nothing functional depends on them. The
`IsItPUMA` ID probe reads back all ones (`$FF`), so a 660AV takes the Clifton
path.

## VRAM and the display

2 MB at `$50100000`, installed both as direct page-table entries (so CPU
accesses are fast) and as a bus-resolver host region (so the 040 table walker
and any DMA reach it by physical address). The `'Nano'` probe at
`base + 2 MB − 8` sizes it. `av_substrate.display` exposes the scanout;
geometry is the Hi-Res mode's 640×480 with the depth from PCBR.

## Testing

`tests/unit/suites/civic/` pins the serial codec (with explicit per-slot
placement checks), the direct-longword access style, the inverted and computed
slots, the four sense drive patterns, Sebastian's quad protocol with
auto-increment and bank select, the depth code driving format/stride/clut_len,
and the VBL ack dance including its PSC line.

The integration suite's `av-video` row proves the whole path end to end from
the ROM's side: after the slot PrimaryInit runs, the screen is 640×480,
`ScrnBase` points inside the CIVIC VRAM aperture, and the framebuffer is not
blank.

## Not modelled

Video input (the whole VDC path — `VDCClk` parks the clock off and `VDCInt`
reads idle, so the `vdig` component never opens), the convolution/flicker
filter, TV-out via Mickey, interlaced modes, and depth/mode switching beyond
what the depth code implies. The per-mode timing values are accepted and
stored but nothing derives geometry from them — the dossier notes that what
the hardware counts is nowhere explained, so treating them as opaque
signatures is the documented recommendation.
