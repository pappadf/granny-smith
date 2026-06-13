# Macintosh IIci

The **Macintosh IIci** ("Aurora", 25 MHz 68030, March 1989) is the first
of the RBV/MDU-class colour Macs. Granny Smith models it in
[src/machines/iici.c](../src/machines/iici.c) /
[iici_internal.h](../src/machines/iici_internal.h).

Architecturally the IIci is **"the IIcx with VIA2 replaced by the RBV
chip and built-in video reading from the slot-$B framebuffer
aperture."** It shares the IIcx's 68030 + integrated PMMU, the
ASC / SCC / SWIM / NCR 5380 peripheral set, the classic VIA1-based ADB +
RTC path, and the same canonical I/O addresses — so the implementation is
the IIcx dispatcher with three changes.

## What differs from the IIcx

| Aspect | IIcx | IIci |
|--------|------|------|
| 2nd VIA | VIA2 @ `$50F02000` | **RBV** @ `$50F26000` (see [docs/rbv.md](rbv.md)) |
| Video | NuBus Display Card 8•24 (slot $9) | **Built-in RBV video**, framebuffer at `$FBB08000` |
| VDAC | (on the card) | Bt450 @ `$50F24000` |
| CPU clock | 15.6672 MHz | 25.0 MHz |
| ROM base | `$40000000` (256 KB universal) | `$40800000` (512 KB dedicated, checksum `0x368CADFE`) |
| SCSI IRQ | via VIA2 | via the RBV (`scsi_set_irq_callback` → `RvSCSIRQ`) |
| Soft power-off | VIA2 PB2 | RBV `RvPowerOff` (`RvDataB` bit 2) |
| `via_count` | 2 | 1 |

Everything else (SCSI pseudo-DMA, ASC, SWIM, SCC, ADB/RTC on VIA1, the
Mode-24 NuBus slot aliasing) matches the IIcx.

## I/O island map (`$50F0xxxx`)

The MDU decoder places I/O at the same addresses as the IIcx GLUE; the
dispatcher uses a `$3FFFF` mirror mask (wider than the IIcx's `$1FFFF`)
so RBV/VDAC decode distinctly from the SCSI windows:

| Device | Offset | Notes |
|--------|--------|-------|
| VIA1 | `$00000` | ADB + RTC (classic path, no Egret) |
| SCC | `$04000` | |
| SCSI pseudo-DMA / regs / blind | `$06000` / `$10000` / `$12000` | NCR 5380 |
| ASC | `$14000` | |
| SWIM | `$16000` | |
| VDAC | `$24000` | Bt450 RAMDAC (CLUT) |
| RBV | `$26000` | combined video / IRQ / control register file |

VIA2 (`$02000`) is *not* present — the RBV supersedes it.

## Built-in video

The IIci's built-in video is presented as a NuBus pseudo-card
([builtin_rbv_video.c](../src/core/peripherals/nubus/cards/builtin_rbv_video.c))
seated at the internal NuBus slot `$B` — the slot whose address space
holds the framebuffer. Unlike the SE/30's slot-$E framebuffer it has no
declaration ROM: the boot ROM drives the video from the hard-coded
`VideoInfoMDU` record.

- The framebuffer buffer (1 MB) is registered at the slot-$B aperture
  base `$FBB00000`; `display.bits = fb + $8000` so the ROM's screen base
  `$FBB08000` (and its Mode-24 alias `$00B08000`) resolve into it.
- The default monitor sense is `6` (binary 110 = 13" RGB) → 640×480, at
  1/2/4/8 bpp selected via `RvMonP`. The CLUT is programmed through the
  VDAC and surfaces via `display.clut`.
- The slot-0 video VBL interrupt (`RvIRQ0`) is asserted once per frame
  in the card's `on_vbl`; the boot ROM polls `RvSInt` bit 6 for it during
  video init.
- v1 models the framebuffer pointer + depth/CLUT only — the RBV's
  cycle-stealing DMA-from-RAM (and the associated "RAM bandwidth tax")
  is not modelled.

## Interrupt routing

`iici_update_ipl` maps sources to 68030 IPL the same way as the IIcx,
with the RBV at level 2 in place of VIA2:

```
NMI  -> IPL 7
SCC  -> IPL 4
RBV  -> IPL 2     (combined slot / SCSI / sound interrupt)
VIA1 -> IPL 1
```

## Status

The IIci boots stock System 7.1 from floppy to a working Finder desktop.
The integration test
[tests/integration/iici-boot](../tests/integration/iici-boot) boots the
System 7.1 "Disk Tools" floppy and pixel-matches the Finder desktop.

Deferred (not v1): the optional Parity Generator Card, the 32 KB L2
cache card, sound IRQ delivery (`RvSndIRQ` is unwired, matching the
IIfx's ASC), and user-installable NuBus cards in slots `$C/$D/$E`.

## See also

- [docs/rbv.md](rbv.md) — the RBV chip.
- [src/machines/iicx.c](../src/machines/iicx.c) — the closest template.
