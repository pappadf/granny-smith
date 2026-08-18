# Onboard video — AMIC timing + Ariel II over a DRAM framebuffer

`src/machines/pdm/ariel.c`.  PDM has no VRAM: built-in video scans a
framebuffer in ordinary system DRAM, **forced to physical address 0** by
the boot allocator (an HMC fetch-engine requirement), through AMIC's
timing generator and the Ariel II CLUT/DAC.  The software model is
"Sonora with no VRAM" — AMIC implements the LC III's Sonora video
register model and the shipping ROM drives it with the same
`.Display_Video_Apple_Sonora` driver.  Sources: the Developer Note
(Table 3-10 timing, Table 3-8 depths), the 8100 schematics, and the
shipping ROM's Sonora driver/PrimaryInit access idioms.

## Registers

- **Video control** (`$50F28000`, decoded in amic.c → here):
  `+0` mode — bit 7 = blank, low bits = monitor code (6 = Hi-Res 640×480
  66.67 Hz, the wired monitor; codes 1/2/9/11/13 decode their raster
  geometry too); `+1` depth code 0–4 = 1/2/4/8/16 bpp; `+2` sense
  (amic.md "Monitor sense"); `+3` vtest.
- **Ariel II** (`$50F24000`): address / data (R,G,B per entry,
  auto-increment after blue) / config (low 3 bits mirror the depth code,
  bit 3 "master mode") / key.  Register-compatible with the LC's
  V8/Ariel DAC.

## Scanout model

The substrate `.display` hook publishes a `display_t` over host RAM at
physical 0 — always inside the soldered bank, which the HMC never
relocates, so the window is host-contiguous on every model and every RAM
configuration.  Geometry derives entirely from mode + depth (stride =
width × depth / 8, packed — there is no scan-base or stride register on
this hardware); the VBL raster marks the framebuffer dirty every frame.

While the blank bit is set (or the mode code selects no timing set, e.g.
the `$9F` power-on state), the descriptor presents a black stub buffer
instead: a real monitor shows black without syncs, and the alternative —
blanking the raster bytes — would write live guest memory.

At reduced depth the hardware feeds the DAC eight index lines with the
unused low lines driven high, so a pixel value *i* reads CLUT entry
`(skip−1) + i×skip` with `skip = 256/2^depth` — the V8/Sonora convention
(the driver programs `$7F` white / `$FF` black at 1 bpp).  The model
materializes that window into the descriptor's palette; 16 bpp bypasses
the CLUT.  Gamma is software-side (the driver pre-corrects CLUT writes);
no monitor response curve is modeled.

Everything here is derived presentation state, rebuilt from the register
file on init, reset, and checkpoint restore — the framebuffer bytes are
guest RAM and ride the RAM image.

`pdm-rom-ladder` L20 matches the boot's gray desktop (640×480×8, the
`$00`/`$FF` checker dither through the driver's gamma-corrected CLUT)
as a screen golden.
