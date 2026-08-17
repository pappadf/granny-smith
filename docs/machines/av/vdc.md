# The AV video digitizer — DMSD, VDC, and the capture path

The AV Quadras digitize composite/S-video into a **separate frame buffer at
`$50200800`** inside CIVIC's VRAM, which Sebastian overlays onto the graphics
image at display time. Three pieces cooperate, and all of the emulator's side
lives in [src/machines/av/vdc.c](../../../src/machines/av/vdc.c) /
[vdc.h](../../../src/machines/av/vdc.h) plus the video-in slots in
[civic.c](../../../src/machines/av/civic.c):

| Piece | Role | Reached through |
|---|---|---|
| **SAA7191B** "DMSD" | analog CVBS/Y-C → digital YUV, NTSC/PAL/SECAM | I²C slave `$8A` (write) / `$8B` (read) |
| **SAA7186** "VDC" | windowing, decimation, YUV→RGB, chroma keyer, VRAM output port | I²C slave `$B8` / `$B9` |
| **CIVIC** | the VRAM port the VDC writes through: timing window, row stride, clock gate, field interrupt | serial registers at `$50036000` |

The I²C bus is **not** on the CPU bus. It hangs off Cuda and is driven with
pseudo-command **`$22` (`RdWrIIC`)** — see [cuda.md](cuda.md).

Contract and register semantics:
the AV video-in hardware notes (ROM-verified against the
disassembled `'vdig'`/`'i2c '` components, plus both Philips datasheets).

## The chips are register files plus two status bytes

The guest's `'i2c '` component keeps its own RAM shadow of both register
files and serves subaddressed *reads* from it, so on real hardware only the
two status bytes ever travel the wire. The models are therefore write-sinks —
25 DMSD registers (`$00`–`$18`), 17 VDC registers (`$00`–`$10`), subaddress
auto-increment on multi-byte writes — plus:

| Status | Value the model returns |
|---|---|
| DMSD `$8B` | `STTC` mirrors the programmed `VTRC` bit; a connected source reads `HLCK` = 0, `FIDT` = 1 (60 Hz), `CODE` = 1 (colour) → `$A1`. Nothing plugged in reads `HLCK` = 1 → `$C0`. `HLCK` is what `VDGetCurrentFlags` turns into `digiInSignalLock`. |
| VDC `$B9` | `$10 \| OEF<<1 \| SVP` — the version nibble **must** read `%0001` (the only documented value), `OEF` is the field parity just delivered, `SVP` mirrors `VPE`. |

Subaddressed reads *are* served from the register files anyway: the System
Enabler 088 build of the vdig disables its shadow read cache with an
unconditional branch, so those reads do reach the wire on the software this
family actually boots.

## The frame engine

A scheduler event at NTSC field cadence (59.94 Hz). Each firing, when
**`VDCClk` == 0** (clock on), **`BusSize` == 0** (32-bit mode — the master
switch; 64-bit graphics mode has no video port) and **VDC `$00` `VPE`** = 1
(VRAM port enabled), it:

1. pulls the current host frame (§ host sources below) as 640×480 RGBA —
   the SAA7191B is the *square-pixel* variant whose NTSC active picture is
   exactly that, so the host frame plugs in where the DMSD output would be
   and all guest-visible geometry stays a function of the VDC registers;
2. applies the VDC window and decimation — `XO`/`XS`/`XD` horizontally,
   `YO`/`YS`/`YD` vertically, in field lines, with the window origin at the
   NTSC active-picture corner (left 16, top field-line 15);
3. packs to the `FS` format and writes into VRAM at offset `$100800` with the
   `VidInSize` stride (0 → 1024, 1 → 1536 bytes);
4. latches the CIVIC field interrupt.

**The writer always writes when the engine runs**, even with no source (black
fields). This is not politeness: Enabler 088's liveness probe stamps
`$0001FEFF` at `$50200804`, runs one field and fails if the value survives, so
a model that raises interrupts without touching VRAM makes the vdig conclude
the hardware is broken.

Formats: `FS` = `00` is 1-5-5-5 ARGB two pixels per longword with **α = 0** —
Apple ships with the chroma keyer disabled (lower limit > upper limit for both
U and V), so every captured pixel carries α = 0, and that is the state in
which the overlay demonstrably works. `FS` = `11` is 8-bit luminance with the
`$10` `MCT` bit selecting non-inverse (1) or inverse (0) polarity.

Field storage follows `OF`: `00` writes both fields onto *alternate* VRAM rows
(interlaced), `01`/`1x` writes consecutive rows.

## The field interrupt shares CIVIC's VBL line

There is no separate PSC source. The VDC and VBL both raise **PSC-VIA2
slot-interrupt bit 6** (active low, IPL 2); the guest disambiguates through
the Slot Manager queue — CIVIC's handler at priority 255 checks `VBLInt` and
passes, the vdig's `INT_SERVICE` at 200 checks `VDCInt`. So `civic.c` asserts
the line when *either* latch is pending, and the two acks are independent:

| Register | Behavior |
|---|---|
| `VDCInt` `$008` | live flag, **active LOW** — bit 0 = 0 means "interrupt pending" (the inverse of `VBLInt`) |
| `VDCClr` `$00C` | ack: write 0 (clear + disarm) then 1 (re-arm) |
| `VDCEnb` `$010` | 1 enables the field interrupt |
| `VDCClk` `$018` | **1 = clock OFF**; gates the whole engine. `INT_SERVICE` writes 1 to *freeze* the buffer while it copies a field out, so this must stop writes immediately |
| `VidInSize` `$014` | row stride, 0 = 1024 / 1 = 1536 bytes |
| `BusSize` `$04C` | 0 = 32-bit (video-in possible), 1 = 64-bit graphics-only |

`VDCClk` is also the camera lifecycle signal: transitions call
`gs_video_in_state`, which the browser turns into attaching or stopping the
`MediaStreamTrack`, so the camera light is on only while the guest captures.

## Host video sources — `machine.videoin`

| Path | Meaning |
|---|---|
| `machine.videoin.source` | `none` (default — nothing plugged in) · `pattern` · `file` · `host` (the platform webcam) |
| `machine.videoin.connected` | read-only; true when the selected source reports a signal — this is what the DMSD `HLCK` bit answers |
| `machine.videoin.fields` | read-only; fields the engine has written since power-on |
| `machine.videoin.pattern` | select the built-in deterministic test pattern |
| `machine.videoin.load <path>` | load a 640×480 PNG and select it |

The **pattern** is a pure function of the checkpointed frame counter — colour
bars, a gradient band that scrolls with the frame clock, and a binary
frame-counter strip — so it is byte-exact across runs and across
checkpoint/restore. That is what CI's goldens ride on; the webcam is
browser-only and never appears in a golden.

The default is `none`, so a machine that nobody plugs a camera into behaves
exactly as it did before this landed: the vdig still opens, and reports no
signal lock.

`host` reaches the browser through the `gs_video_in_*` seam in
[src/core/system.h](../../../src/core/system.h) — weak defaults model "no
camera", and [src/platform/wasm/em_camera.c](../../../src/platform/wasm/em_camera.c)
overrides them with a double-buffered frame slot pair in the shared wasm heap
(the audio-ring pattern inverted; see [../../guide/web.md](../../guide/web.md)).

## Checkpointing

The two register files, the source selection, the field counter and a loaded
PNG frame are checkpointed; the captured pixels ride along for free inside
VRAM. The webcam is host-ephemeral by design — like audio output it is outside
checkpoint scope, and on restore the frame slots simply repopulate.

## Not modelled

- **The YUV output formats** (`FS` = `01`/`10`) — the `'yuv2'` compressed
  capture path. Fields are dropped with a log line; the shipping driver only
  reaches these through QuickTime compressed capture.
- **The VBI / vertical-bypass region** (`VS`/`VC`, VDC `$09`–`$0B`). Those
  lines would come back as unscaled 8-bit greyscale at full line width
  regardless of `FS`; Video Monitor programs the region but does not read it.
- **The decimation and vertical filters** themselves. `AFS`/`HF`/`VP` are
  accepted and stored; sampling is nearest-neighbour, so the *geometry* the
  guest programs is exact but the filtering is not.
- **Analog fidelity**: brightness (the DMSD's H-sync/clamp reposition), hue,
  saturation and sharpness are register latches with no effect on pixels.
  Contrast is not implemented on real hardware either (returns `digiUnimpErr`).
- **PAL/SECAM geometry** — the host frame contract is NTSC 640×480.
- **S-video vs composite** beyond the status bits the auto-detect probes.
- **Per-pixel key-colour gating in the overlay** (`vdTypeKey`, ≤ 8 bpp
  graphics): the video window wins unconditionally inside its rect, so a
  Finder window dragged over a playthrough window would not clip it. The
  dossier leaves that path untraced (video-in.md §11).
- **Sound of any kind.** Capture is mute — the data path never involves the
  DSP3210 or Singer, and QuickTime movie recording with audio is a separate
  (much larger) project.

## Known open question, inherited from the dossier

**Greyscale polarity.** Apple programs the VDC `MCT` = 1 ("non-inverse
monochrome") yet `CivicVideoCLUTSet` loads a *descending* 32-entry ramp for
8-bpp video-in, which composes to a negative image unless something else
inverts. The model implements `MCT` literally. The 16-bpp ARGB path — the one
the shipping driver actually uses — is unaffected.

**VRAM partitioning** on a 1 MB-VRAM configuration is undocumented: the
video-in buffer sits in the *second* megabyte of the 2 MB aperture, yet the
driver's table has non-zero 1 MB entries. Our AV machines model 2 MB VRAM, so
this only bites if a 1 MB option is ever profiled.
