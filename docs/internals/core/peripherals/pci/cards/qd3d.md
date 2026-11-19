# The QuickDraw 3D Accelerator Card model

`src/core/peripherals/pci/cards/qd3d.c` — the card: PCI identity, the BAR, the packet parser, the
control-program walker and the pixel-output DMA. `qd3d_raster.h` / `qd3d_raster.c` — the scanline renderer
behind a raster seam shaped like the Voodoo2's, and the backends that decide where it runs.

The Apple QuickDraw 3D Accelerator Card is a PCI bus-master rendering co-processor with no frame buffer: it
reads a frame's geometry out of host memory, renders it a scanline at a time and writes the pixels back over
PCI into the screen's VRAM or a host back buffer. The hardware — its PCI identity, registers, packet and
record formats, descriptor lists and frame protocol — is documented in
[`quickdraw3d.md`](../../../../../reference/hardware/pci/cards/quickdraw3d.md) (§2 the interface, §3 the behaviour, §4.3–§4.6 the driver and plug-in); this
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

**Timing.** A write of register `0x20` walks the whole frame at once — the control program and every object
record are read then — and arms a `frame-done` event for the time the card would take: 20 µs plus 50 ns per
output pixel (20 Mpixel/s) plus 100 ns per object. At that event the frame's pixels are streamed through
the descriptor list into host memory, and the interrupt is raised if DMA control bit 14 is set. The time is
computed from the frame's size, never from how long the host took to render it, so the guest sees the same
thing on every backend (§3). The interrupt must not be synchronous with the register write: the driver
marks the card running only just before it writes register `0x20`, but its interrupt handler is the only
place the flag is cleared.

## 2. Key types & files

| Type | Role |
|---|---|
| `qd3d_t` | the card: renderer registers, DMA control and command pointer, configuration register `0x40`, packet-parser state, the interrupt line, the in-flight frame's descriptor list, frame counters, the object cache and the texture memory (all checkpointed as one block), then the runtime-only DMA cursor, the renderer's target and its backend |
| `qd3d_dma_t` | the pixel-output channel's place in its descriptor list |
| `q3_obj_t` | one decoded object: kind, header, scanline range, 13.3 vertices, attribute planes, texture slot and size, bitmap tile |
| `q3_frag_t` | one fragment of one pixel: depth and premultiplied colour |
| `q3_draw_state_t` | the snapshot of every register the renderer reads: background, CSG table, mip level bases, perspective and 16-bpp flags |
| `q3_cmd_t` | one command: start a frame, add an object, render a scanline |
| `q3_target_t` | the renderer's own state: the frame's objects and live list, the fragment lists, the frame's output stream; it reads the card's texture SRAM |
| `q3_raster_t` | a backend: the draw-state ring and, for `thread`, the worker and its command ring |

The flow is `bar_write32` → `port_write` (the packet parser) → `internal_write` → `frame_start` on register
`0x20` → `frame_walk` (the program walk) → `control_block` / `obj_decode` → `q3_raster_submit` → … →
`frame_done_event` → `frame_output` (fence, then `dma_stream`).

## 3. The raster seam

The split mirrors the Voodoo2's (`voodoo2_raster.h`, see [`voodoo2.md`](voodoo2.md), "The raster seam"). The
card is the **producer**: it owns the register file and every bus-master access, and turns the walk into
commands. The **executor** (`q3_raster_execute`) renders the commands into a target it owns; it never sees
the card struct and never touches host memory, so a worker thread cannot race the guest. Each command names
a slot in a ring of draw-state snapshots, re-captured only when a register in the snapshot is written (by
the port, by a control block, or the 16-bpp bits of DMA control). Two invariants keep the backend
guest-invisible:

1. **Timing is analytic.** A frame completes at the time its size gives it, and its pixels reach host
   memory then, whichever backend drew them.
2. **The target is read only after a sync.** The producer fences before it streams a frame's output, writes
   the texture SRAM, checkpoints, or swaps the backend.

A backend is only where the executor runs:

| Backend | |
|---|---|
| `sw` | inline at submit — the **normative** one |
| `null` | as `sw`, with object commands dropped: every frame is the background |
| `thread` | the **default**: a bounded single-producer, single-consumer ring drained by one worker pthread (in the browser a Web Worker created on demand), the Voodoo2's design; the fence waits for the ring to drain |

`pci_option="raster=thread|sw|null"` picks one per boot; `machine.pci.slot[N].card.renderer.raster`
reports it and, assigned, swaps it in place. `-DGS_QD3D_RASTER_DEFAULT='"sw"'` makes a build default to the
walker and `-DGS_QD3D_THREAD_BACKEND=0` compiles the thread backend out. `GS_QD3D_STATS=1` logs the thread
backend's counters at teardown (commands, fences and how long they waited, the worker's CPU time). There is
no user-facing option: unlike the Voodoo2 the card has one rasteriser, the emulator's own.

Because queue order is submission order and every read fences first, the thread backend's output is
byte-identical to the walker's. On Gerbils! (87 380 pixels, 219 objects a frame) the walker takes about
3.6 ms of host time per frame; on the thread it overlaps the CPU emulation, and the CPU thread waits for it
on about half the frames, for 1.6 ms on average.

**Output.** The output channel writes byte by byte, as a PCI bus master into device memory always has; the
memory layer's `memory_write_bytes_slow` resolves each device page once and calls its byte handler without
re-deciding the route per byte, which is most of what streaming a frame into VRAM cost.

## 4. Behaviour

- **Program walk.** From register `0x20`, each record's link is the word at `4 · index`, its first word
  after the `N` links (`N` from configuration register `0x40` once the driver has set it, else 1) decides
  between an object (opcode 3) and a control block. A link of 0, or four million records, ends the walk;
  65 536 zero-length descriptors in a row stop the output channel.
- **Control blocks.** `Y` writes to `0x04`–`0x07` are latched only when the address's low bits equal this
  card's index; `0x30` sets the width; **scanline start** submits the latched current line, if any;
  **scanline end** does nothing; any other register write lands in the register file.
- **Active list.** Objects are appended as they are walked (the executor keeps the frame's objects and a
  list of the live ones' indices) and dropped once a rendered scanline is below
  their last; the walk order is the plug-in's (each scanline group's objects newest first), which is also
  the order fragments of equal depth arrive in — the first one kept wins against a later opaque one.
- **Textures.** A port word written to texture memory is stored quantised to the 5-6-5 SRAM format
  (`texel_store`), after a fence, since the renderer may still be reading the SRAM; alpha is dropped. The page in a textured header is turned into a slot and a size from
  register `0x0C` and the slot-class registers, and a texel of level *L* is read from the level-base
  registers exactly as the plug-in laid it out, wrapping u and v. A page with no loaded map drops the
  object.
- **Cut short.** A frame still in flight when DMA control bit 12 (reset) is written or the next frame
  starts gets no interrupt, but its pixels still go out, then. The descriptor list is the one `DMACMDPTR`
  named when the frame started.
- **Reset.** DMA control bit 12 also drops the interrupt and resets the packet parser, keeping both SRAMs;
  bit 13 drops the interrupt.

## 5. Object-model / shell surface

`machine.pci.slot[N].card.renderer`:

| Member | |
|---|---|
| `frames` | frames rendered since power-on |
| `objects` | objects in the last frame |
| `lines` | scanlines the last frame rendered |
| `dma_control` | DMA control |
| `position` | configuration register `0x40` |
| `irq` | the interrupt line |
| `raster` | the backend: `thread`, `sw` or `null`; assigning one swaps it in place |
| `read(offset)` | a renderer register by byte address |

Log category `qd3d`: level 1 errors (a bad descriptor, a runaway program), 2 oddities, 3 one line per frame
when it completes (with an FNV-1a hash of its pixels, a cheap identity for comparing runs), 4 every
register write, 5 every descriptor and triangle, 6 every object record in hex. The renderer itself never
logs: it may be running on the worker.

## 6. Checkpointing

The plain-data head of `qd3d_t` up to `dev` — registers, channel and parser state, the in-flight frame's
descriptor list, counters and both SRAMs — is one checkpoint block, after a fence. A frame in flight has its
rendered output after it (a byte count, then the bytes), since those pixels reach host memory only when it
completes; with no frame in flight the count is 0. A pending `frame-done` event is a registered scheduler
event and is restored with the scheduler's queue; the interrupt line is re-derived from the restored state.

## 7. Testing

`tests/integration/tnt-qd3d` (extended tier) starts with a device-level row that needs no disk: it checks
the PCI header, then pokes two hand-built 16 × 4 frames through the BAR — a control program, two box-like
objects and a descriptor list in RAM — once plain and once as the CSG difference A − B, on each backend in
turn (thread, sw, and null, which leaves only the background), and checks that nothing is written at the go
write, that the interrupt rises once the frame has run, and every pixel the card writes back. The other rows run Apple's unmodified driver and plug-in on a pm8500 with
Mac OS 7.6 and QuickDraw 3D 1.6 from `system_7_6_170mb_24ac_qd3d.dmg`: the driver's bring-up (position
register, object-cache free list), RaveEngineInfo listing "Apple PCI Hardware", Gerbils! rendering through
the card, TextureEyes' textured square, the QuickDraw 3D Viewer drawing the Teapot model, Gerbils at
thousands of colours (the 16-bpp path), and — from `apps/virtual_wings_qd3d.dmg` on the 21" monitor — Virtual Wings'
cockpit view. That row shows the card, not QuickDraw 3D's software engine, draws it: the card has drawn nothing
before the cockpit opens, then completes frames of over 200 scanlines from loaded texture maps, and the view matches
a golden of textured, hazy terrain (with the game's acceleration switched off, the card draws no frame and the
software engine's flat-shaded view differs from the golden in nearly half its pixels). Goldens are
regenerated with `TEST_VARS="REGEN=1"`.

`tests/e2e/web2-specs/qd3d-thread.spec.ts` checks the browser build: the renderer comes up on its Web
Worker (`raster` is `thread`, not a fallback to `sw`), and a one-scanline frame poked through the BAR lands
in memory when it completes.

Beyond the test, the image's Sneak3view draws the same scenes through the card and the software engine side
by side, which is how the coverage rule, the texture orientation and the blend were checked during
bring-up.

## 8. Known debts

- **Equal-depth order** follows the walk order, not a rule of the card's.
- **Overflow mode** (register `0x34 = 0`) changes nothing; the model never runs out of object storage.
- **Multiple cards** are modelled per card (each renders its own scanlines from its index), but no test seats
  more than one.
- The pixel-exact rules (edge rule, interpolation, the kd/ks formula, rounding) are the model's own choices;
  the hardware's are open questions in the reference page.

## 9. See also

- [`quickdraw3d.md`](../../../../../reference/hardware/pci/cards/quickdraw3d.md) — the hardware.
- [`voodoo2.md`](voodoo2.md) — the other ROM-less 3D card, a display pass-through rather than a co-processor,
  and the raster seam this one copies.
