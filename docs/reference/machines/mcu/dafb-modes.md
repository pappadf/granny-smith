# DAFB display modes and VRAM depth ceilings

The **D**irect **A**ccess **F**rame **B**uffer is the built-in video controller
in the Quadra 700, 900 and 950. The 950 uses a revised configuration that adds a
16-bit "Thousands" mode, so its capabilities differ from the 700/900.

The tables below are Apple's published **maximum** colour depth per display
timing and VRAM fit, transcribed from period documentation (sources at the
foot). Lower supported depths can also be selected. They describe **the
hardware**, not this emulator — see [What we actually
implement](#what-we-actually-implement) before treating a row as a test target.

## Quadra 700 and 900

Resolutions are given horizontal × vertical, though one Apple table prints some
of them reversed.

| Display timing            | Resolution      | 512 KB VRAM | 1 MB VRAM   | 2 MB VRAM   |
| ------------------------- | --------------: | ----------: | ----------: | ----------: |
| 12-inch RGB               | 512 × 384       | 8-bit       | **24-bit**  | **24-bit**  |
| 12-inch monochrome        | 640 × 480       | 8-bit       | 8-bit       | 8-bit       |
| 13-inch RGB / VGA         | 640 × 480       | 8-bit       | 8-bit       | **24-bit**  |
| Super VGA                 | 800 × 600       | 8-bit       | 8-bit       | **24-bit**  |
| 15-inch portrait mono     | 640 × 870       | 4-bit       | 8-bit       | 8-bit       |
| 16-inch colour            | 832 × 624       | 8-bit       | 8-bit       | **24-bit**  |
| Two-page monochrome       | 1152 × 870      | 4-bit       | 8-bit       | 8-bit       |
| 21-inch colour            | 1152 × 870      | 4-bit       | 8-bit       | 8-bit       |
| PAL, convolution enabled  | Interlaced PAL  | unavailable | 8-bit       | 8-bit       |
| PAL, no convolution       | Interlaced PAL  | 8-bit       | 8-bit       | **24-bit**  |
| NTSC, convolution enabled | Interlaced NTSC | unavailable | 8-bit       | 8-bit       |
| NTSC, no convolution      | Interlaced NTSC | 8-bit       | 8-bit       | **24-bit**  |

The Quadra 700 shipped with a minimum of **512 KB** VRAM; the Quadra 900's
minimum configuration was **1 MB**.

## Quadra 950

Shipped with **1 MB of 80 ns VRAM**, expandable to **2 MB** via four 256 KB VRAM
SIMMs. The revised DAFB adds 16-bit colour (32,768 colours), which mainly
benefits the larger displays.

| Display timing        | Resolution | 1 MB VRAM  | 2 MB VRAM  |
| --------------------- | ---------: | ---------: | ---------: |
| 12-inch monochrome    | 640 × 480  | 8-bit      | 8-bit      |
| 12-inch RGB           | 512 × 384  | **24-bit** | **24-bit** |
| 13-inch RGB / VGA     | 640 × 480  | **16-bit** | **24-bit** |
| 16-inch colour        | 832 × 624  | **16-bit** | **24-bit** |
| 15-inch portrait mono | 640 × 870  | 8-bit      | 8-bit      |
| 19-inch colour        | 1024 × 768 | 8-bit      | **16-bit** |
| 21-inch monochrome    | 1152 × 870 | 8-bit      | 8-bit      |
| 21-inch colour        | 1152 × 870 | 8-bit      | **16-bit** |

The 950 also supports VGA, 800 × 600 SVGA, NTSC and PAL timings; the cited Apple
summary does not enumerate every VRAM/depth combination for those.

## The "24-bit" nuance

On the Quadra 700/900 each 24-bit pixel occupies a full **32-bit longword** in
VRAM. "24-bit colour" is therefore effectively a **32-bpp framebuffer**, which
is why 640 × 480 at 24-bit needs 2 MB rather than fitting into 1 MB:

```
640 x 480 x 4 bytes = 1,228,800 bytes  -> over 1 MB, under 2 MB
```

Read the table with that in mind: a "24-bit" cell is a statement about VRAM
consumption as much as about colour.

## What we actually implement

These ceilings are the hardware's, and most of the table is **not reachable in
this emulator today**. The binding constraint is monitor sense, not VRAM:

- DAFB currently honours **only monitor sense 6** (13-inch RGB, 640 × 480).
  Senses 0/1/2 program nonsense rasters — tracked as an open defect, guarded by
  `suite-quadra`'s `q700-dafb-senses` milestone row, which reports red nightly.
- `suite-quadra`'s 832 × 624 cell is blocked for the same reason.

So this file is best read as the target the DAFB work is aiming at, and as the
measure of how much of the mode space is still missing. When sense handling is
fixed, the rows to add come straight off these tables — with the VRAM column
picking which depth each Quadra config can actually claim.

## Sources

- Macintosh Quadra 700 developer note —
  <https://www.macdat.net/files/pdf/apple/developer_notes/macintosh_quadra_700.pdf>
- Macintosh Quadra 950 developer note —
  <https://www.macdat.net/files/pdf/apple/developer_notes/macintosh_quadra_950.pdf>
- TIL 09091, "Macintosh Quadra: VRAM and Pixel Depth" —
  <https://www.savagetaylor.com/TIL/TIL09091.pdf>
- TIL 10211, "Macintosh Quadra 950: Video Support" —
  <https://www.savagetaylor.com/TIL/TIL10211.pdf>
- TIL 10992, "Macintosh Quadra 700, 900: Built-In Video (3/93)" —
  <https://www.savagetaylor.com/TIL/TIL10992.pdf>
