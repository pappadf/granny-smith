# ATI Rage 128 GL

The ATI Rage 128 GL as a retail Macintosh PCI display card — the Rage
Orion, Xclaim VR 128 and Nexus 128 — modelled in
[`src/core/peripherals/pci/cards/rage128.c`](../../../../../../src/core/peripherals/pci/cards/rage128.c)
(the card),
[`rage128_2d.c`](../../../../../../src/core/peripherals/pci/cards/rage128_2d.c)
(its 2D draw engine),
[`rage128_cce.c`](../../../../../../src/core/peripherals/pci/cards/rage128_cce.c)
(its Concurrent Command Engine) and
[`rage128_raster.c`](../../../../../../src/core/peripherals/pci/cards/rage128_raster.c)
(its 3D engine), sharing
[`rage128_priv.h`](../../../../../../src/core/peripherals/pci/cards/rage128_priv.h).
The chip's hardware reference is
[`rage-128.md`](../../../../../reference/hardware/pci/cards/rage-128.md);
this page covers the emulator's model, what drove each choice, and what is
not modelled yet. Like the Mach64 GX, the card is driven by its **own**
firmware: Open Firmware runs the FCode in its expansion ROM to build the
node, and Mac OS loads the `.Display_Rage128` ndrv that ROM publishes. The
model answers registers and nothing else.

| | |
|---|---|
| **Card kind** | `rage128` (`card_class = "display"`, `requires_prom`) |
| **PCI ID** | `1002:5245` ("RE", Rage 128 GL PCI), class `$030000`, revision `$00` (the first silicon; the production A22 value is unknown and every guest read of it is logged) |
| **ROM** | the Xclaim VR 128's `113-57406-108` (FCode 1.69, node `ATY,Rage128v`; catalogue default) or the Nexus 128's `113-57502-103` (`ATY,Rage128n`); identities in [`expansion-rom.md`](../../../../../reference/hardware/pci/expansion-rom.md) |
| **BARs** | BAR0 64 MB prefetchable memory, BAR1 256 B I/O, BAR2 16 KB memory, ROM 128 KB |
| **Memory** | 16 MB (Rage Orion, Xclaim VR 128); `memory=32m` for the Nexus 128 |
| **Monitor** | `monitor=vga` (default: a VGA monitor answering DDC), `vga_noddc`, `13in_rgb`, `21in_rgb` |

## 1. Responsibilities & design

**Status.** Milestones 4b–4f of the Rage 128 work: the PCI face, the
four apertures and their byte-order swappers, the register file and its
config mirror, the PLL file, monitor sense and DDC, the palette, the CRTC
turned into a display descriptor at 8, 15/16 and 32 bpp, the hardware
cursor, and the VBLANK interrupt — enough for the card's ndrv to run
System 7.6 at every depth it offers — then the 2D draw engine, the
Concurrent Command Engine that feeds it packets, and the 3D engine the
CCE's vertex packets drive. Every engine runs to completion when started,
so `GUI_STAT`, `PM4_STAT`, `PC_GUI_CTLSTAT` and `PC_NGUI_CTLSTAT` report
idle.

**Driven by the FCode's needs.** The model was written against a decode of
the Xclaim FCode's probe and `open`, and three facts from that decode shaped
it:

- **Probe goes through the I/O BAR.** Before Open Firmware maps BAR0 and
  BAR2, the FCode maps BAR1 and reaches the *whole* register file through
  `MM_INDEX`/`MM_DATA` at I/O `$0`/`$4` — the clocks, the memory controller,
  sense, DDC and the mode set all happen there. The card's `reg` property
  does not list BAR1, but Open Firmware 1.0.5 and 2.4 both size and assign
  it, so it is a real BAR (not a strapped decode, as the Mach64's I/O was).
- **The sense path is chosen by a VSYNC loopback.** The FCode drives
  `CRTC_V_SYNC_STRT_WID` bit 23 (vertical-sync polarity) and watches
  `GPIO_MONID` pad 3 follow it. A VGA cable does not loop it back, so the
  FCode reads DDC and falls back to sense code `$717` (VGA, 640 × 480 at
  60 Hz); an Apple cable does, and only then are the three Apple sense
  lines read. The `monitor=` option picks the cable.
- **`get-msecs` must advance.** The probe calibrates a busy loop and then
  waits about a second for DDC plus 32 ms steps for each clock; on the G3
  that is long enough that keystrokes typed during it are lost (the test
  waits for the probe's end, below).

## 2. Key types & files

- `rage128_t` — the card: the 2048-dword register file (`reg[]`,
  little-endian values as the chip holds them), the 64-entry PLL file, the
  AGP command and power-state latches, VRAM, the palette and its two
  auto-incrementing indices, the monitor, the EDID block and the DDC slave,
  and the scanout state.
- `r128_monitor_t` — a cable: whether it loops VSYNC back (Apple sense),
  whether an EDID EEPROM answers on it, and its sense straps in the Mach64
  card's three-step extended model.
- `r128_ddc_t` — the bit-banged I²C slave.
- `r128_cce_t` — the CCE: the microcode RAM and its two address counters,
  and two packet streams being gathered (`main` for the ring and the PIO
  FIFO, `ind` for the indirect buffer, which a ring packet can call
  mid-stream).
- `scripts/rage128/rage128_regs.py` — the register name ↔ offset table the
  constants here are checked against.

## 3. Behaviour/algorithms

**Apertures.** One register file, four ways in:

| Face | Bytes | Order |
|---|---|---|
| BAR2 + 0 | register aperture 0, `$0000`–`$1FFF` | little-endian |
| BAR2 + 8 KB | register aperture 1 | little-endian, or host order with `CONFIG_CNTL.APER_REG_ENDIAN` |
| BAR1 | `$00`–`$FF` (the non-GUI block, incl. `MM_INDEX`/`MM_DATA`) | little-endian |
| `MM_INDEX`/`MM_DATA` | anything; `MM_INDEX` bit 31 (`MM_APER`) selects VRAM | little-endian |

The manual names one swap bit for "the register apertures" without saying
which; it is applied to aperture 1 only, the copy the manual says exists for
the Power Macintosh, so a little-endian driver on aperture 0 is never
disturbed. Byte and halfword accesses reach the lanes they address ("add 1,
2 or 3 to the address", RRG Table 2-2), carried into the register write as a
lane mask so side-effect registers know which lanes moved.

BAR0 is two 32 MB linear apertures over the same VRAM. Each has its own
swapper (`APER_0_ENDIAN`, `APER_1_ENDIAN`): 16 bpp swapping XORs a CPU byte
address with 1, 32 bpp swapping with 3, so a big-endian host's pixel store
lands as the little-endian pixel the CRTC scans. Beyond the memory size the
aperture is empty (reads `$FF`, writes dropped). The `CONFIG_APER_*` and
`CONFIG_REG_*` registers are computed from the assigned BARs: the low bit of
the aperture-1 and register-aperture-1 base fields is hardwired to one, so
the second copies sit at BAR0 + 32 MB and BAR2 + 8 KB.

**Configuration space.** The generic header plus: `AD_STEPPING` hardwired in
the command register, `MIN_GNT` 8, `INTERRUPT_LINE` resetting to `$FF`, and
the capability list at `$50` (AGP 1.0, then power management at `$5C`,
which terminates it). Registers `$F00`–`$FFF` are a read-only mirror of the
whole of it.

**PLL.** `CLOCK_CNTL_INDEX` bits 5:0 select a PLL register and bit 7
(`PLL_WR_EN`) gates writes; `CLOCK_CNTL_DATA`'s lanes reach the register's
bytes. The PPLL atomic-update bit 15 of `PPLL_REF_DIV` and `PPLL_DIV_0..3`
clears at once — the FCode polls it, for up to half a second, after every
divider write. No clock is derived from the dividers yet; the raster runs at
the host frame rate.

**Monitor sense and DDC.** `GPIO_MONID` is four open-drain pads against a
pull-up: a pad reads low when the card drives it (`EN=1, A=0`), when the
monitor's ties pull it, or (pad 1, SDA) when the DDC slave holds it. Pads
0/2/1 are Apple's SENSE0/1/2 (crosswise, because pads 2 and 1 are also DDC
SCL and SDA); pad 3 is the VSYNC loopback. The DDC slave is level-driven: it
detects START/STOP from SDA moving while SCL is high, samples on SCL's rising
edge, changes SDA only while SCL is low, and serves a synthesised EDID 1.3
for a 640 × 480 VGA monitor at `$A0`/`$A1`. `DAC_CNTL.DAC_CMP_OUTPUT`
reports a load whenever a monitor is attached and the comparators are
enabled.

**Scanout.** `CRTC_H_TOTAL_DISP` bits 23:16 + 1 characters wide,
`CRTC_V_TOTAL_DISP` bits 26:16 + 1 lines, `CRTC_PITCH` × 8 pixels per line,
`CRTC_OFFSET` in **bytes** (the manual says 64-bit words; the FCode writes
`$8000` and publishes BAR0 + `$8000` as its frame buffer). The raster is on
when `CRTC_EN`, `CRTC_EXT_DISP_EN`, display requests enabled and
`CRTC_EXT_CNTL.CRTC_DISPLAY_DIS` clear. 8 bpp scans VRAM in place through the
palette (`DAC_MASK` applied); 15, 16 and 32 bpp are byte-swapped into a
compose buffer each frame, since the display layer takes big-endian pixels.

**Hardware cursor.** A 64 × 64 map at `CUR_OFFSET`, each line eight bytes
of AND bits then eight of XOR bits, leftmost pixel in bit 7 (SDK §4.4):
AND/XOR `00` colour 0, `01` colour 1, `10` transparent, `11` the complement
of the pixel beneath. `CUR_HORZ_VERT_OFF` says where in the map the visible
part starts; `CUR_HORZ_VERT_POSN` (vertical in 10:0, horizontal in 26:16)
places it. The colours are always 24-bit RGB; at 8 bpp the composite takes
the nearest palette entry. The cursor is an overlay into `compose`, never
written to VRAM; with it on, even 8 bpp is presented through `compose`.

The card's ndrv keeps the System's arrow in this cursor. It moves only on
real pointing-device motion: Mac OS feeds `DrawHardwareCursor` from the
Cursor Device Manager, so the harness's low-memory mouse placement
(`machine.adb.mouse.move … "global"`, which writes `MTemp`/`RawMouse`/
`Mouse`) moves hit-testing but not the arrow on screen. `"hw"` mode is real
ADB motion and moves both.

**The 2D draw engine** (`rage128_2d.c`). Register-programmed, and run to
completion when initiated, so `GUI_STAT` always reads idle with the FIFO
empty. `DP_GUI_MASTER_CNTL` loads the data path (destination, brush and
source datatypes, byte pixel order, ROP3, source) into `DP_DATATYPE` and
`DP_MIX`, re-defaults the source and destination surfaces and scissors from
`DEFAULT_OFFSET`/`DEFAULT_PITCH`/`DEFAULT_SC_BOTTOM_RIGHT` unless its
leave-alone bits say otherwise, clears the colour-compare functions or opens
the write mask on request, and sets `DP_CNTL`'s directions (RRG §7.5). The
combined registers (`DST_Y_X`, `SRC_Y_X`, `DST_PITCH_OFFSET`,
`SC_TOP_LEFT`, …) write their halves.

An operation runs when its last dimension arrives: `DST_HEIGHT`, or any
combined register carrying the height (`DST_HEIGHT_WIDTH`,
`DST_WIDTH_HEIGHT`, `DST_HEIGHT_WIDTH_8`, `DST_HEIGHT_WIDTH_BW`,
`DST_HEIGHT_Y`) or the width (`DST_WIDTH_X`, `DST_WIDTH_X_INCY`); a line
runs on `DST_BRES_LNTH`. The reference names only `DST_WIDTH_BW` an
initiator; the rest follow the ATI lineage the Mach64 model pins, and every
operation is logged at video level 3 so a driver that initiates some other
way shows up.

Per pixel: the source (VRAM rectangle, or host data streamed through
`HOST_DATA0..7`/`HOST_DATA_LAST`; colour, or mono expanded to
`DP_SRC_FRGD/BKGD_CLR` with an optional leave-alone background), the brush
(solid, 8×8/8×1/1×8/32×1/32×32 mono, 8×8/8×1/1×8 colour), the full ROP3,
the colour compare, the write mask, the scissors (inclusive). The register
reference and SDK Table 4-2 disagree on the sense of the destination
compare codes; the reference ("4 = draw when equal, 5 = draw when not
equal" for both) is followed. Overlapping blits follow `DP_CNTL`'s
directions with the coordinates of the first pixel walked, as drivers
supply them. Lines are Bresenham on the `DST_BRES_*` terms, with
`BRES_SIGN` deciding a zero error term and `DST_LAST_PEL` the end pixel.
Scaled blits, trapezoids and the 24 bpp quirks are not modelled.

**The Concurrent Command Engine** (`rage128_cce.c`). On silicon a
microcoded processor turning command packets into register writes; here
the packets are executed natively, with the effect SDK appendix F gives
them, and the microcode the guest uploads (256 `DATAH`/`DATAL` pairs from
`PM4_MICROCODE_ADDR`) is only stored and identified: its CRC-32 against
ATI's published image (`regs.microcode` reads `known`, `unknown` or
`none`). Packets: type 0 writes `COUNT+1` registers from `BASE_INDEX`
(or one register, `ONE_REG_WR`), type 1 two registers, type 2 is a
one-dword filler, type 3 an operation. The 2D operations become exactly the
register writes a PIO driver would make into the 2D engine —
`DP_GUI_MASTER_CNTL` and the SETTINGS block (pitch/offsets, scissors, the
brush packet, `BRUSH_Y_X`), then a trajectory and an initiator — for
`PAINT`, `PAINT_MULTI`, `BITBLT_MULTI` (the walk directions chosen so an
overlapping copy is safe), `TRANS_BITBLT`, `POLYLINE` (each segment leaves
its end pixel to the next; the last draws it only with `DST_LAST_PEL`),
`POLYSCANLINES`/`PLY_NEXTSCAN` (span ends exclusive), `HOSTDATA_BLT`,
`NEXTCHAR` and `SET_SCISSORS`. The 3D packets hand vertices to the 3D
engine: `3D_RNDR_GEN_PRIM` carries them inline; `3D_RNDR_GEN_INDX_PRIM`
names a vertex list in AGP space (`VLOFF`, an offset in the AGP window)
that the vertex walker reads in order or by 16-bit indices, and
`NEXT_VERTEX_BUNDLE` adds indices to the last one. `SMALL_TEXT`, the scaler
packets and `LOAD_PALETTE` are skipped by their count, logged once each.

Packets arrive three ways, chosen by `PM4_BUFFER_CNTL`'s mode: through
`PM4_FIFO_DATA_EVEN/ODD` in the PIO modes; from a ring of
2^(n+1) dwords at `PM4_BUFFER_OFFSET` in the bus-master modes, fetched from
`PM4_BUFFER_DL_RPTR` up to `PM4_BUFFER_DL_WPTR` once the microengine is
free-running, with the new read pointer written back to
`PM4_BUFFER_DL_RPTR_ADDR` (not with `NOUPDATE`); and from the indirect
buffer, `PM4_IW_INDSIZE` dwords at `PM4_IW_INDOFF`, run when `INDSIZE` is
written — by PIO or by a type-0 packet in the ring. The ring and the
indirect buffer are card addresses: below 32 MB the frame buffer, above it
the "AGP" window, which on this PCI card reaches host memory through the
**PCI GART** (`PCI_GART_PAGE`: a table of 8192 little-endian page addresses,
4 KB pages; bit 0 disables it) or, with the GART disabled, linearly from
`AGP_BASE`. Every host access is `pci_dma_read/write`, so it needs both the
PCI command register's `BUS_MASTER_EN` and `BUS_CNTL.BUS_MASTER_DIS`
clear. The fetch runs to completion inside the `WPTR` write: a driver
reading `RPTR` straight after finds the ring drained, and `PM4_STAT`
reports the mode's FIFO share free and nothing busy. `SOFT_RESET_GUI`
drops a packet half gathered and a host-data operation half fed.

**The 3D engine** (`rage128_raster.c`). The CCE decodes each vertex
(`VC_FORMAT`: X, Y, Z and the optional RHW, diffuse and specular colours
as floats or packed ARGB, fog, two texture-coordinate sets, RHW2) and
hands the batch over with its primitive type (`VC_CNTL`: points,
independent lines, polyline, triangle list, fan, strip). A snapshot of the
state is taken per batch. The 3D context copies alias 2D state, as the
reference says and `rage128_2d.c` implements: `DST_PITCH_OFFSET_C` is the
render target, `SC_*_C` the scissors, `PLANE_3D_MASK_C` the write mask, and
`DP_GUI_MASTER_CNTL_C` the data path whose destination datatype is the
colour buffer's format (565, 1555, 4444, 8888, 332, 8-bit). A
`DP_GUI_MASTER_CNTL` write without `GMC_3D_FCN_EN` turns the engine off.
`SCALE_3D_CNTL` and `MISC_3D_STATE_CNTL_REG` share their blend, alpha-test
and fog-table fields, and a write to either updates both.

Per pixel, in the order the SDK gives: texel fetch (both units: wrap,
mirror, clamp or border per axis; nearest, bilinear, mip-nearest,
mip-linear, trilinear; `PRIM_TEX_0` is the *smallest* map, as the CCE
supplement and Mesa's uploads say and the SDK's chapter 6 does not), the
two combine stages, texture lighting, specular, fog (vertex fog from the
specular alpha, or the 256-entry table indexed by Z), colour key, alpha
test, stencil (24-bit Z only; `ZFAIL` is "stencil passes, Z fails", the
SDK and Mesa over the supplement), Z, blend (`ALPHA_COMB_FCN` with the
thirteen factors), dither or round, the ROP3 and brush (Mesa's polygon
stipple is a 32 × 32 mono brush), the write mask.

The conventions the manuals leave open are chosen and stated in the file
header: vertices snap to the sub-pixel grid after `WINDOW_XY_OFFSET`;
pixel centres at (x + ½, y + ½) with a top-left fill rule; S/T
perspective-correct, colours, fog and Z affine; Z = z·(2^N − 1),
truncated; the mip level from the pixel's neighbours less
`LOD_BIAS`/128; texel centres at (i + ½)/size; 8-bit products rounded;
a 4 × 4 ordered dither; DDA lines with the end pixel left off. Tiled
surfaces are addressed linearly — consistent between the engines, not
with a tiled view through an aperture (logged once). Colour buffers in
AGP space and the palettised and YUV texture formats are not drawn.

**Page flips.** A `CRTC_OFFSET` write while the CRTC runs is a flip: the
display keeps scanning the old base until the next vertical blank, and
`CRTC_GUI_TRIG_OFFSET` (bit 30 of `CRTC_OFFSET` and `CRTC_OFFSET_CNTL`)
reads 1 until then; `CRTC_OFFSET_LOCK` holds it longer, and
`CRTC_OFFSET_FLIP_CNTL` (take it on the next line) applies it at once, the
model having no lines. With the CRTC off — a mode set — the offset applies
immediately.

**Interrupts.** `GEN_INT_STATUS` latches VBLANK and VSYNC every frame
whether or not they are enabled (write 1 to clear); `GEN_INT_CNTL` gates
the INTA line, which is level and held until acknowledged. `CRTC_STATUS`
bit 0 is the live blank, bit 1 the since-last-cleared latch.

## 4. Object-model / shell surface

`machine.pci.slot[N].card` carries `framebuffer` (the shared display node,
nominated as `machine.screen.source`), `monitor` (`id`, `ddc`,
`apple_sense`) and, under the advanced category, `regs` (`vram_size`,
`crtc_gen_cntl`, `config_cntl`, `microcode`, `cce_packets`, `prims3d`, `read(offset)`
without side effects, `pll(index)`).

## 5. Checkpointing

The register file, PLL file, AGP/power latches, palette and indices, the
DDC slave's state, a 2D host-data operation in flight, the 3D fog table,
the CCE (microcode RAM and partially gathered packets) and VRAM. The ROM travels in the slot's checkpoint part.
A checkpoint from a card of another memory size fails the size-tagged read
loudly.

## 6. Testing

- `tests/integration/rage128-prom` — the ROM catalogue (milestone 4a).
- `tests/integration/suite-tnt`, row `pm9500-76-rage128` — System 7.6 from
  the MESH disk to the Finder on the card (chime, mount, goldens), then
  256 colours and millions from the Control Strip, each a golden.
- `tests/integration/rage128-2d` (tier `unit`) — the 2D engine with no
  guest code: the row assigns the BARs itself, drives the registers and
  asserts VRAM equalities for fills, blits (overlapping included),
  transparency, mono and colour host data with the big-endian swap,
  brushes, the ROP3, the write mask, the scissors and lines.
- `tests/integration/rage128-cce` (tier `unit`) — the CCE the same way:
  the microcode RAM, packet types 0–3 through the PIO FIFO,
  `SOFT_RESET_GUI`, a bus-mastered ring through the PCI GART (gated by
  `BUS_MASTER_DIS` and `BUS_MASTER_EN`, wrapping, `RPTR` written back or
  not), the indirect buffer called from the ring and by PIO, the
  GART-disabled linear window, and the 2D packets — VRAM and `RPTR`
  equalities. Guest RAM is not usable before POST programs the memory
  controller, so the row stands the card's own VRAM, reached as a PCI peer
  through BAR0, in for host memory.
- `tests/integration/rage128-3d` (tier `unit`) — the 3D engine through the
  CCE with Mesa's state values: the fill rule's edges, Gouraud and flat
  shading, culling, Z (test and mask), stencil (replace, equal, increment),
  blending, the alpha test, vertex fog, a nearest-filtered textured quad,
  `MODULATE`, a two-unit lightmap-style stage, the vertex walker's list and
  indexed walks through the GART, lines, points and a 565 target — every
  one a VRAM equality.
- `tests/integration/tnt-pci-rage128` — the config header before any
  instruction; the node Open Firmware 1.0.5 builds from the FCode, read
  back with `.properties` over the serial console, for each cable and for
  the Nexus ROM; the same node under the beige G3's Open Firmware 2.4; the
  ndrv driving the grey desktop in thousands of colours; the apertures,
  swappers and the index pair.

## 7. Known debts

- No pixel clock: refresh is the host's, and PLL dividers are stored only.
- `CRTC_VLINE` interrupts, display tiling (`CRTC_TILE_EN`) and packed
  24 bpp are not modelled.
- No guest exercises the 2D engine or the CCE yet: the ROM ndrv does not
  accelerate, and the `ATI Graphics Accelerator` and `ATI Rage 128 3D
  Accelerator` that would need a Mac OS 9.x image with the ATI stack
  (media-gated). Which CCE mode the Mac driver picks is therefore unknown.
- The CCE does not interpret microcode: a guest that uploads its own and
  depends on behaviour other than appendix F's would diverge.
- `SMALL_TEXT`, `SCALE`, `TRANS_SCALE` and `LOAD_PALETTE` are not executed.
- The 3D engine's undocumented conventions (fill rule, interpolator
  precision, LOD, dither matrix, fog-table indexing) are chosen, not
  measured; a real-card capture would pin them. The texture palette, YUV
  textures, edge anti-aliasing, tiled layouts and AGP colour buffers are
  not modelled, and the rasteriser is a single synchronous software walker
  (no worker-thread or WebGPU backend yet).
- The revision byte is `$00` until a real card is read.

## 8. See also

- [`rage-128.md`](../../../../../reference/hardware/pci/cards/rage-128.md) — the chip.
- [`pci.md`](../../pci.md) — the bus, config header, bus mastering.
- [`expansion-rom.md`](../../../../../reference/hardware/pci/expansion-rom.md) — PROM identity and the catalogue.
