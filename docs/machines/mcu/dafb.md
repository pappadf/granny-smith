# DAFB — Direct Access Frame Buffer (Quadra built-in video)

The **DAFB** is the Quadra family's stand-alone video controller,
implemented in [src/machines/mcu/dafb.c](../../../src/machines/mcu/dafb.c) /
[dafb.h](../../../src/machines/mcu/dafb.h). One chip page, four
sub-blocks, two fixed CPU apertures:

| Aperture | Address | Size |
|---|---|---|
| VRAM | `$F9000000` | 2 MiB CPU window (installed VRAM may be smaller) |
| Registers | `$F9800000` | low `$400`, longword-decoded (`>>2`) |

Register sub-blocks: **DAFB core** `+$000`, **Swatch** CRTC `+$100`,
**AC842/AC842a** RAMDAC `+$200`, **DP8531** clock generator `+$300`.
Apple's own equates (`HardwarePrivateEqu.a`) pin the core offsets:
`VidBaseHi $0`, `VidBaseLo $4`, `RowWords $8`, `Config $10`, `Sense $1C`,
`Reset $20`, `Swatch_Mode $100`. Unknown registers stay accept-and-log
with readback.

## Geometry (Swatch)

From the ROM's captured mode-set sequence, cross-checked against the
reference [R]:

- `width = (HFP − HAL) / pixel_divide`, `height = (VFP − VAL) / 2`
- `h_total = HPIX + 2`, `v_total = VFPEQ / 2`
- frame-buffer base `= (VidBaseHi & $FFF) << 9 | (VidBaseLo & $F) << 5`
- The DP8531 nibble sequence reproduces the ROM's 640×480 program as
  exactly **25.175 MHz**; the System's driver re-programs a 66.9 Hz
  Apple-timed mode.

## Monitor sense (`$1C`)

Three rules, all cross-checked against Apple's `DAFBDriver.a` [A]:

1. drive bits are **active-low** (bit clear = drive that line low);
2. reads return the line states **inverted** (`DAFBReadSenseLines` does a
   `NOT.b`);
3. **the drive latch wakes up tristated (`$7`)** — the ROM's very first
   `$1C` read (before writing anything) is the standard-sense identify.
   A zero reset value reads as sense code 0 and the ROM silently
   configures a 21-inch two-page display.

A plain 13-inch monitor is code 6; the cross-drive tuples produce the
`extendedHR $2B` extended sense naturally.

## RAMDAC: AC842 vs AC842a (Q950)

The ACDC address register (`$200`) selects which PCBR the config register
(`$220`) reaches: 0 → **PCBR0**, 1 → **PCBR1**. On the plain AC842 there
is no PCBR1 — a "PCBR1" write clobbers PCBR0, which is exactly what
PrimaryInit's presence probe tests (write PCBR0=`$06`, write "PCBR1"=0,
read PCBR0 back: `$06` ⇒ AC842a, 0 ⇒ AC842) [A]. On the AC842a
(`dafb_set_ac842a`):

- PCBR1's low nibble is read-only manufacturer/revision (0 here — the
  driver loads the sparse Trans5to8 CLUT);
- **PCBR1 = `$C0` switches direct color to big-endian x555 16 bpp**
  ("Thousands"; written only when entering the FifthVidMode);
- PCBR1 also latches the acdcPCS clock-select bit (unmodeled).

`DAFB_Test` (`$2C`) carries the chip version in bits 11:9
(`dafb_set_version`): 0 on Q700/Q900, **3 on the Q950 ("DAFB 3")** — the
driver's 33 MHz path shifts by 9 and compares with DAFB3Vers to decide
16 bpp is always allowed.

## TurboSCSI DRQ readback

DAFB channel control registers expose the 53C96's live DRQ in **bit 9**
(`dafb_set_scsi_drq_query`; channel 0 = internal, channel 1 = the tower's
external controller). The functional model always satisfies the
acknowledge timing immediately — see
[scsi_53c96.md](../../core/peripherals/scsi_53c96.md).

## Interrupts and scanout

The Swatch's programmed timing drives the video interrupt (level output →
VIA2 PA6 through the family /SLOTIRQ aggregate); a 60.15 Hz fallback
covers the pre-mode-set window. Scanout renders from VRAM at the
programmed geometry/depth; PCBR0 direct-color + PCBR1 `$C0` selects the
x555 16 bpp pixel format (the same format the 8•24 GC path renders).

## State

`dafb_checkpoint` covers the full register file, the CLUT **including the
in-flight write phase** (a partial RGB triplet is real state), both
PCBRs, the clock program, and VRAM.
