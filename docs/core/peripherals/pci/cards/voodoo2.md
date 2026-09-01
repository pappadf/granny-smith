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
- **3c — the rasteriser (landed):** both triangle paths (host setup and
  the on-chip setup engine with strips/fans/culling), the full pixel
  pipeline in the p.15 order, two TMUs with single-pass multitexture,
  all fifteen texture formats with the NCC/palette decode, the packed
  mip-chain address calculator (pinned by the spec's three worked
  examples), the LFB's write formats/lanes/bypass-vs-pipeline split,
  `fastfillCMD`, the five statistics counters, and the raster-backend
  seam ([`voodoo2_raster.h`](../../../../../src/core/peripherals/pci/cards/voodoo2_raster.h))
  with the software walker as the normative default and a null backend
  pinning the analytic-timing invariant.  See "The fill convention and
  the divergence list" below.
- **3d — pass-through and display (landed):** `v2_drives_monitor()` —
  the one place the pass-through state is computed (fbiInit0[0] under
  the driver's 1-= -drives convention, gated by video reset, software
  blanking and the output enables, with the fbiInit6[29:28] override) —
  and `v2_display()`, which returns the descriptor while driving and
  NULL while passing through, flagging `shape_dirty` on both edges.
  The scanout raster converts the displayed buffer to the display
  layer's big-endian `PIXEL_16BPP_565` at the card's own edge;
  swapbufferCMD retirement flips the front/back mapping.  Checkpoint
  restore of a socketed card fixed in the GENERIC layer
  (`system_restore` now re-seeds the PCI staged picks from the restored
  record, the exact NuBus parallel that was already there).  web2's
  Expansion Slots section gains the one non-display socket picker.
  Gate: `tnt-pci-voodoo2-display` — take/release against a live 7.6
  desktop with the release matching the pre-takeover golden
  byte-identically, and a mid-drive checkpoint restoring to the same
  framebuffer checksum.
- **3e — guest software (Mac OS 8.1 + `Quake 3Dfx`) — detection
  complete; rendering blocked on a non-card launch issue.**  Running
  the SHIPPED 3dfx driver exposed three model gaps, all closed: the
  **CMDFIFO engine** (V2 §11 — the proposal wrongly believed it off the
  Glide 2.x path; Mac Glide's whole render path uses it and polls
  `cmdFifoRdPtr` for room), **TMU send-config** (`trexInit1[18]` — how
  software counts a board's TMUs, decoded from Glide's own use), and a
  **calibratable dither** (Glide builds un-dither tables by rendering
  all 256 values and requiring unique 4×4 tile sums — the previous
  dither plateaued at the range ends and failed the driver's own
  self-test).  With those, the real `3DfxGlideLib2.x` completes
  **`grSstQueryHardware` end to end against the model**: Name-Registry
  match on `pci121a,2`, Memory Space enabled by the driver itself, the
  full §1.6 bring-up, ICS5342 detection, FBI memory sized 4 MB, dither
  calibration, TMU census and both TMUs' memory sensed — gated by
  `tests/integration/tnt-voodoo2-glide` (tier extended, media-gated on
  the machine-local image).
  **Known limit:** after the query succeeds, Quake parks in an event
  loop with the display blanked *before* calling `grSstWinOpen`.  The
  identical wait occurs with NO card seated (the A/B control), so it is
  a guest-environment launch issue — likely an invisible launcher UI or
  an unrelated subsystem wait on the blanked screen — not a card
  behaviour.  The rendered-frame half of the milestone (grSstWinOpen,
  in-game frames) is blocked on diagnosing that guest-side wait.

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

## The fill convention, and the divergence list (milestone 3c)

**The triangle fill rule is CHOSEN, not known.**  V2 §7.2, on the
TRIANGLE command, reads in full: *"TO BE COMPLETED. SEE THE SST-1
PROGRAMMING GUIDE FOR A DETAILED EXPLANATION"* — and nobody holds that
guide (proposal §8 Q1, §10 item 1).  The convention this rasteriser
implements, derived from what the spec does give (the p.36 area formula
and sign, 12.4 vertices, the sub-pixel rule):

- sample points at pixel integer coordinates;
- inside/outside by edge functions oriented by the **command's** area
  sign (a sign that disagrees with the geometry draws nothing);
- half-open top-left inclusion, so triangles sharing an edge tile with
  no seam and no double-drawn pixel (asserted in the gate);
- parameter iteration from vertex A's truncated position.

Every pinned pixel value and count in `draw.script` records what *this*
rasteriser draws — regression anchors, not hardware conformance.  The
divergences, each deliberate and localised:

| # | Divergence | Where | Why |
|---|---|---|---|
| 1 | Idle is inverted: work completes at issue, busy reads 0 | `v2_status` | the faithful failure mode is an unbounded guest spin (§8 Q3) |
| 2 | The fill rule above | `v2_sw_triangle` | not specified at any price; §8 Q1 |
| 3 | Dither thresholds (classic Bayer 4×4/2×2, remainder-threshold rule) | `v2_pack565` | the spec names the modes but not the matrices |
| 4 | Per-pixel LOD from analytic texel-space steps | `v2_texture_chain` | the LOD arithmetic is Bruce-spec material nobody holds (§8 Q4) |
| 5 | 1/W→4.12 float-depth normalisation | `v2_depth_float` | the exact normalisation is not in our material |
| 6 | Fog table indexing (4-bit exponent + 2 mantissa bits, no inter-entry interpolation) | `v2_pixel_pipe` | normalisation unspecified; no held client uses fog |
| 7 | Float-mirror→fixed conversion truncates toward zero | `v2_float_to_latch` | conversion rounding unspecified |
| 8 | DAC power-on PLL N/P bytes (M bytes are the detection signature and exact) | `v2_dac_reset` | only the M values are documented |
| 9 | trexInit0/1 opaque except the second-RAS size gate | `v2_tmu_addressable` | V2 p.85: "FIXME. See Bruce spec" |
| 10 | The internal 33-entry gamma CLUT (clutData) is stored but not applied at scanout | `v2_display_update` | identity scanout keeps goldens self-consistent; revisit if a client visibly gammas |

What is *not* on this list, because the hardware behaviour is documented
and implemented faithfully: sub-pixel correction mutating the start
latches per FIFO read (so resend-less triangles drift — §8 Q9, asserted),
the reversed LFB transform orders, texture-memory aliasing under the
sizing probes, and the initEnable gates.

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
