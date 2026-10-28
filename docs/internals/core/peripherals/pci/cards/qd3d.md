# The QuickDraw 3D Accelerator Card model

`src/core/peripherals/pci/cards/qd3d.c` — the whole card: PCI identity, the BAR, the packet parser, the
control-program walker, the scanline renderer and the pixel-output DMA.

The Apple QuickDraw 3D Accelerator Card is a PCI bus-master rendering co-processor with no frame buffer: it
reads a frame's geometry out of host memory, renders it a scanline at a time and writes the pixels back over
PCI into the screen's VRAM or a host back buffer. The hardware — its PCI identity, registers, packet and
record formats, descriptor lists and frame protocol — is documented in
[`quickdraw3d.md`](../../../../../reference/hardware/pci/cards/quickdraw3d.md) (§2.8–§2.14, §3.11, §4.7); this
page covers how the emulator models it. The card id is `qd3d`: `machine.boot ... pci_card="qd3d"` seats it in
the first PCI socket of any machine with one.

## 1. Responsibilities & design

The model is **behavioural**. Apple's driver and plug-in never read back anything the card computes — not
the object cache, not the texture memory, not any intermediate state — so the model owes them three
things: the host interface (three registers, the packet port, the interrupt), a faithful walk of the
control program, and the pixels that come out of the descriptor list. How the ASIC gets there (16-pixel
segments, the four-layer Z-ARGB store and its overflow re-rendering, the object cache) is not modelled; the
object-cache writes the driver makes are stored and otherwise ignored.

What the card renders is decided per scanline from the objects active on it:

- **Triangles** (Gouraud and textured) are set up from their three 13.3 vertices with plane equations for
  every attribute — depth, alpha, colour or kd/ks, u, v and the mip footprint. A triangle covers the
  scanlines whose centres lie in `(yMin, yMax]` (the plug-in's own bucketing rule) and, on each, the pixel
  centres from its left edge up to but excluding its right edge.
- **Textured** triangles divide u, v and the footprint by the interpolated 1/w per pixel when the mode
  register's perspective flag is set, choose a mip level from the footprint, and filter trilinearly — always,
  as the card does. The texel is modulated by the grey diffuse intensity (1.7 fixed point, 128 = 1.0) and
  the grey specular intensity is added.
- **Bitmap tiles** draw their set bits in the tile's colour.
- Every covered pixel collects a **fragment**; fragments are kept front to back (depth 1/w, larger nearer,
  with the perspective flag, else z, smaller nearer), an opaque fragment discards everything behind it, and
  at most eight layers are kept per pixel. The layers are then composited front to back as premultiplied
  colour — RAVE's default blend, the only one the card offers (the plug-in ignores the Blend tag) — over the
  background register.
- **CSG**: walking the sorted layers front to back from outside every operand, a CSG operand's front face
  sets its bit in a 5-bit inside state and its back face clears it; the state indexes the truth table in
  register `0x24` (bit *s* is the answer for state *s*, operand *n* being state bit *n*), and an operand's
  surface is kept only where the answer changes. CSG surfaces are never opaque (the plug-in does not mark
  them), so they never hide the layers behind them.

The finished scanline goes out as 32-bit `xRGB` or, with the 16-bpp bits set, 16-bit `x1555` words,
big-endian, through the descriptor list.

**Timing.** A write of register `0x20` renders the whole frame at once, DMA included, then arms a
`frame-done` event for the time the card would take — 20 µs plus 50 ns per output pixel (20 Mpixel/s) plus
100 ns per object — whose handler raises the interrupt if DMA control bit 14 is set. The interrupt must not
be synchronous with the register write: the driver marks the card running only just before it writes
register `0x20`, but its interrupt handler is the only place the flag is cleared.

## 2. Key types & files

| Type | Role |
|---|---|
| `qd3d_t` | the card: renderer registers, DMA control and command pointer, configuration register `0x40`, packet-parser state, the interrupt line, frame counters, the object cache and the texture memory (all checkpointed as one block), then the runtime-only DMA cursor, active list and scanline buffers |
| `qd3d_obj_t` | one decoded object: kind, header, scanline range, 13.3 vertices, attribute planes, texture slot and size, bitmap tile |
| `qd3d_frag_t` | one fragment of one pixel: depth and premultiplied colour |
| `qd3d_dma_t` | the pixel-output channel's place in its descriptor list |

The flow is `bar_write32` → `port_write` (the packet parser) → `internal_write` → `frame_start` on register
`0x20` → `render_frame` (the program walk) → `control_block` / `obj_decode` → `render_scanline` →
`dma_stream`.

## 3. Behaviour

- **Program walk.** From register `0x20`, each record's link is the word at `4 · index`, its first word
  after the `N` links (`N` from configuration register `0x40` once the driver has set it, else 1) decides
  between an object (opcode 3) and a control block. A link of 0, or four million records, ends the walk;
  65 536 zero-length descriptors in a row stop the output channel.
- **Control blocks.** `Y` writes to `0x04`–`0x07` are latched only when the address's low bits equal this
  card's index; `0x30` sets the width; **scanline start** renders the latched current line, if any;
  **scanline end** does nothing; any other register write lands in the register file.
- **Active list.** Objects are appended as they are walked and dropped once a rendered scanline is below
  their last; the walk order is the plug-in's (each scanline group's objects newest first), which is also
  the order fragments of equal depth arrive in — the first one kept wins against a later opaque one.
- **Textures.** A port word written to texture memory is stored quantised to the 5-6-5 SRAM format
  (`texel_store`); alpha is dropped. The page in a textured header is turned into a slot and a size from
  register `0x0C` and the slot-class registers, and a texel of level *L* is read from the level-base
  registers exactly as the plug-in laid it out, wrapping u and v. A page with no loaded map drops the
  object.
- **Reset.** DMA control bit 12 cancels a pending completion, drops the interrupt and resets the packet
  parser, keeping both SRAMs; bit 13 drops the interrupt.

## 4. Object-model / shell surface

`machine.pci.slot[N].card.renderer`:

| Member | |
|---|---|
| `frames` | frames rendered since power-on |
| `objects` | objects in the last frame |
| `lines` | scanlines the last frame rendered |
| `dma_control` | DMA control |
| `position` | configuration register `0x40` |
| `irq` | the interrupt line |
| `read(offset)` | a renderer register by byte address |

Log category `qd3d`: level 1 errors (a bad descriptor, a runaway program), 2 oddities, 3 one line per frame,
4 every register write, 5 every descriptor and triangle, 6 every object record in hex.

## 5. Checkpointing

The plain-data head of `qd3d_t` up to `dev` — registers, channel and parser state, counters and both SRAMs —
is one checkpoint block. A pending `frame-done` event is a registered scheduler event and is restored with
the scheduler's queue; the interrupt line is re-derived from the restored state.

## 6. Testing

`tests/integration/tnt-qd3d` (extended tier) starts with a device-level row that needs no disk: it checks
the PCI header, then pokes two hand-built 16 × 4 frames through the BAR — a control program, two box-like
objects and a descriptor list in RAM — once plain and once as the CSG difference A − B, and checks every
pixel the card writes back. The other rows run Apple's unmodified driver and plug-in on a pm8500 with
Mac OS 7.6 and QuickDraw 3D 1.6 from `system_7_6_170mb_24ac_qd3d.dmg`: the driver's bring-up (position
register, object-cache free list), RaveEngineInfo listing "Apple PCI Hardware", Gerbils! rendering through
the card, TextureEyes' textured square, the QuickDraw 3D Viewer drawing the Teapot model, and Gerbils at
thousands of colours (the 16-bpp path). Goldens are
regenerated with `TEST_VARS="REGEN=1"`.

Beyond the test, the image's Sneak3view draws the same scenes through the card and the software engine side
by side, which is how the coverage rule, the texture orientation and the blend were checked during
bring-up.

## 7. Known debts

- **Equal-depth order** follows the walk order, not a rule of the card's.
- **Overflow mode** (register `0x34 = 0`) changes nothing; the model never runs out of object storage.
- **Multiple cards** are modelled per card (each renders its own scanlines from its index), but no test seats
  more than one.
- The pixel-exact rules (edge rule, interpolation, the kd/ks formula, rounding) are the model's own choices;
  the hardware's are open questions in the reference page.

## 8. See also

- [`quickdraw3d.md`](../../../../../reference/hardware/pci/cards/quickdraw3d.md) — the hardware.
- [`voodoo2.md`](voodoo2.md) — the other ROM-less 3D card, a display pass-through rather than a co-processor.
