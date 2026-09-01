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
- **3b — enumeration and bring-up (landed):** the card model in
  [`voodoo2.c`](../../../../../src/core/peripherals/pci/cards/voodoo2.c)
  — config declaration, the vendor block at `$40`–`$57`, the three-face
  aperture, the register file with its gating rules, the ICS5342 DAC and
  PLL, LFB with memory-sizing aliasing, and the idle contract — gated by
  `tests/integration/tnt-pci-voodoo2` (below).
- **3c — the rasteriser:** not yet landed.  Until it lands, draw
  commands are accepted, logged once, and complete "instantly"; the
  guest-visible TMU memory/config probe (which renders through the
  texture pipeline) waits for it.
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

## The model, milestone 3b — what is enforced where

Each rule lives in exactly one place in `voodoo2.c`:

- **Enumeration** is entirely the generic layer's: one declaration (BAR0
  16 MB prefetchable, `rom_size = 0`, subsystem zero, only
  `PCI_CMD_MEM_SPACE` writable). The guest's real Open Firmware sizes
  and assigns the BAR and — this being a ROM-less card — loads no driver
  and leaves Memory Space Enable **clear** (Apple, printed p.97); the
  integration test observes exactly that, then sets the bit itself the
  way the Glide library does through `ExpMgrConfigWrite*`.
- **`initEnable[0]`** gates every `fbiInit*` write at the register
  file's edge (`v2_reg_write`); `initEnable[2]` remaps `fbiInit2` reads
  to the DAC read latch (`v2_reg_read`). `busSnoop0/1` are write-only
  and read zero; `cfgStatus` aliases the live status register so a
  driver can poll before mapping; the undocumented `$C0`/`$E0` writes
  fall through to the generic reserved behaviour (no fault, logged once).
- **The idle contract is inverted from silicon** (documented divergence,
  proposal §8 Q3): work completes synchronously and `status` is
  *composed*, never stored — FIFO fields read empty, busy bits read 0,
  the retrace bit and beam counters derive from the scheduler
  (`v2_scanline`, the `mach64_scanline` idiom) so anything spinning on
  beam position advances, and the swap-pending count retires at the
  frame boundary after issue.
- **Register decode** (`v2_reg_face_*`): wrap aliases discarded, chip
  select routes TMU writes (reads always answer from Chuck), bit 21
  selects the alternate triangle mapping under `fbiInit3[0]`
  (`v2_alt_to_std`, the same generation rule `voodoo2_regs.py --check`
  validates), bit 20 selects the per-access byte swizzle under
  `fbiInit0[3]`. Narrow register accesses are undefined bus behaviour:
  logged once, reads all-ones.
- **The ICS5342** is modelled exactly and alone: the PLL register file
  behind write/read address latches with M-then-P/N data phases, the
  three power-on M values Glide's detection checks (`$79`/`$55`/`$71`),
  and `Fout = 14.318 MHz × (M+2)/(2^P (N+2))`. The AT&T/TI back-door
  probes read the read-mask register and fail their ID compares — one
  family answered correctly beats three answered halfway.
- **Memory aliasing is load-bearing**: LFB addressing maps buffers
  linearly (colour buffer K at K × `memOffset` pages, aux after them,
  row stride `tilesInX`×64 bytes) and wraps modulo the 4 MB
  framebuffer; texture writes wrap modulo the addressable TMU size,
  which `trexInit0`'s second-RAS bit halves — because both Glide and
  sstfb *size memory by watching which addresses alias*.
- **LFB transforms** are two separate functions with the documented
  opposite orders (writes swizzle→wordswap→lanes, reads
  lanes→wordswap→swizzle, V2 p.53/p.56) — deliberately not one helper
  with a direction flag, which is the shape in which the order gets
  inverted by a later edit.

Gate: `tests/integration/tnt-pci-voodoo2` (tier unit) — the config
probe before any instruction runs, the guest firmware's own assignment,
and `glide-init.script`, a step-by-step replay of 3dfx's own bring-up
order with every step's postcondition asserted in place and every wait
bounded. The empty slot's all-ones read is the positive control.

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
