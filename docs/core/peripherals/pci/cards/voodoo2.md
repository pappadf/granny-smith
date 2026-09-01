# 3dfx Voodoo2 (CVG)

Third-party 3D-only **pass-through** PCI accelerator (not an Apple
product; the Mac boards — TechWorks Power3D II, Micro Conversions Game
Wizard — were the PC reference design with Mac drivers and a monitor
pass-through cable). To be modelled in
[`src/core/peripherals/pci/cards/voodoo2.c`](../../../../../src/core/peripherals/pci/cards/voodoo2.c);
design in `proposal-pci-3dfx-voodoo2.md` (PCI Phase 3).

This file adopts, for PCI, the per-card documentation convention NuBus
uses under [`nubus/cards/`](../../nubus/cards/); retro-fitting a
`mach64gx.md` is follow-on work.

| | |
|---|---|
| **Card kind** | `voodoo2` |
| **PCI ID** | `121A:0002`, class `$038000`, revision `$02` (pinned; real revision in `initEnable[15:12]`) |
| **ROM** | **none** — no expansion ROM, no FCode; OF builds a bare `pci121a,2` node and the guest's user-space Glide library claims the card by PCI ID |
| **BAR** | one: BAR0, 16 MB prefetchable memory (registers / LFB / texture faces) |
| **Memory** | option `memory=8m` (2 MB/TMU) or `12m` (4 MB/TMU); 4 MB framebuffer either way |
| **Display** | drives the monitor only while pass-through is taken (`fbiInit0[0]`); never the machine's display device (`card_class = "3d"`) |

## Status

Delivered in milestone groups on one branch (`pci-3dfx-voodoo2`); this
document grows as each group lands.

- **3a — substrate (landed):** `PIXEL_16BPP_565` in the display layer
  (below), the register-name table `scripts/voodoo2/voodoo2_regs.py`,
  this directory.
- **3b — enumeration and bring-up:** not yet landed.
- **3c — the rasteriser:** not yet landed.
- **3d — pass-through and display:** not yet landed.
- **3e — guest software (Mac OS 8.1 + `Quake 3Dfx`):** not yet landed.

## Provenance

The model is documentation-first from 3dfx's own material: the *Voodoo2
Graphics Specification* rev 1.16 (Dec 1999) and 3dfx's own released Glide
2.x source (`glide2x/cvg/`, first-party vendor source — used for sequence
and constants, no code copied). Apple's *Designing PCI Cards and Drivers
for Power Macintosh* (revised 1999) specifies the ROM-less enumeration
path. Linux `sstfb` and MacGLide are cross-check only (hardware facts
cited, never code copied). **No other emulator's device model is used**:
`dingusppc/` and `mame-voodoo/` sit quarantined in the project's
`do-not-read/` directory, and nothing here may be derived from them.

## The 5-6-5 pixel format (milestone 3a)

The Voodoo2's framebuffer is natively 5-6-5 RGB, and the display layer
had no such format (1/2/4/8 indexed, 1-5-5-5, 32-bpp XRGB). Rather than
have the card convert to a neighbouring format — which would throw away
one bit of green on every pixel and make every golden a lossy record —
`PIXEL_16BPP_565` was added to
[`display.h`](../../../../../src/core/peripherals/nubus/display.h):

- big-endian byte order, matching `PIXEL_16BPP_555` (the display layer's
  convention; a card owns any byte-order conversion at its own edge);
- expansion rule: 5-bit channels replicate their top 3 bits
  (`(c5 << 3) | (c5 >> 2)`), the 6-bit green its top 2
  (`(g6 << 2) | (g6 >> 4)`) — in the PNG encoder and `screen.match`
  path ([`debug.c`](../../../../../src/core/debug/debug.c)) and the
  WebGL shader ([`em_video.c`](../../../../../src/platform/wasm/em_video.c));
- the Mach64 GX's `CRTC_PIX_WIDTH = 4` refusal became a real mode
  ([`mach64gx.c`](../../../../../src/core/peripherals/pci/cards/mach64gx.c)),
  which is also what tests it today: `tnt-pci-mach64` programs a 5-6-5
  raster and pins the expansion against fixtures computed independently
  in `make-fixtures.py`, with the same bytes reinterpreted as 5-5-5 as
  the positive control.

## The register-name table (milestone 3a)

`scripts/voodoo2/voodoo2_regs.py` — register offset → name / width /
R-W class / FIFO / pipelined flags, transcribed from V2 spec pp.22-26,
plus the alternate triangle mapping (`fbiInit3[0]` + address bit 21)
from pp.27-29. Used so traces and logpoints name registers instead of
printing offsets. `--check` runs its positive control: the alternate map
is regenerated *by rule* from the standard table (each parameter's
start/dX/dY made adjacent, parameters in start-block order) and must
equal the transcription entry for entry, so a transposed row in either
transcription fails; the gradient blocks are additionally pinned against
the start block, and spot rows are pinned straight off the spec pages.
