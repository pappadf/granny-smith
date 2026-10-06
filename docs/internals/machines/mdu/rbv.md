# RBV — RAM-Based Video chip

The **RBV** ("RAM-Based Video", Apple part 344S1019) is the combined
video-control + interrupt-aggregation ASIC used by the Macintosh IIci
(and, in its *V8* variant, the IIsi / LC family). In Granny Smith it is
implemented as a flat peripheral module:
[src/machines/mdu/rbv.c](../../../../src/machines/mdu/rbv.c) /
[rbv.h](../../../../src/machines/mdu/rbv.h). Hardware reference:
[rbv.md](../../../reference/machines/mdu/rbv.md) (register file §2,
the V8 variant §1.5).

On the IIci the RBV **replaces the VIA2** of the IIcx-family machines: it
lives at physical `$50F26000`, aggregates the slot / SCSI / sound
interrupts into a single 68030 **IPL 2** assertion, owns the
soft-power-off and external-cache control bits, and carries the built-in
video's monitor-sense + depth register. The framebuffer (the bottom of
Bank A in main RAM) is scanned out by the
[builtin_rbv_video](../../../../src/machines/mdu/builtin_rbv_video.c)
NuBus pseudo-card through a Bt450 VDAC at `$50F24000`; the RBV only holds the
depth/monitor register (`RvMonP`) and the slot-0 video VBL interrupt
(`RvIRQ0`).

## Register file

Eight 8-bit registers at small byte offsets from `$50F26000`:

| Offset | Name      | Dir | Purpose |
|--------|-----------|-----|---------|
| `$000` | `RvDataB` | R/W | Control: cache-disable, bus-lock, soft-power-off, cache-flush, NuBus transfer-mode, sound-source, parity-test |
| `$001` | `RvExp`   | R/W | Expansion register (accept-and-log) |
| `$002` | `RvSInt`  | R   | Slot interrupt status (`RvIRQ1..6` = bits 0-5, `RvIRQ0` = bit 6); active-low |
| `$003` | `RvIFR`   | R/W | Interrupt flags: `RvSCSIDRQ` b0, `RvAnySlot` b1, `RvSCSIRQ` b3, `RvSndIRQ` b4, bit 7 = any-enabled-pending (read) / set-clr select (write) |
| `$010` | `RvMonP`  | R/W | Monitor parameters: depth `RvColor1..3` (bits 0-2), monitor-sense `RvMonID1..3` (bits 3-5, read-only), `RvVIDOff` b6, `RvVID3St` b7 |
| `$011` | `RvChpT`  | R/W | Chip-test register (accept-and-log) |
| `$012` | `RvSEnb`  | R/W | Slot-interrupt enable (bits 0-6; bit 7 = set-clr select on write) |
| `$013` | `RvIER`   | R/W | Interrupt enable (bits 0-6; bit 7 = set-clr select on write) |

### Byte-lane aliases

Apple's *shared* VIA2/RBV OS code reaches the IFR and IER not at the
native offsets `$003`/`$013` but at the VIA-register-spaced aliases
`Rv2IFR = vIFR + RvIFR = $1A03` and `Rv2IER = vIER + RvIER = $1C13` (the
IER decode requires A4=1 — an RBV ASIC quirk documented in the mac68k
headers). A/UX 3.0.1's level-2 interrupt handler likewise reads the
slot-interrupt register at `vBufA + RvSInt = $1E02` (the 6522 VIA2's port
A carried the slot lines). The RBV memory interface decodes the native
small offsets and these three aliases, so code written either way reaches
the same register. Undecoded, `$1E02` read `$FF` ("no slot pending") and the
IIci kernel spun in its handler on the built-in video's VBL (`RvIRQ0`).

## Behavioural model (v1)

1. **Interrupt aggregation.** RBV's SCSI / slot / sound interrupts are
   level inputs. `RvIFR` is composed live from the source state on every
   change; the chip asserts a single combined interrupt (→ IPL 2)
   whenever `(RvIFR & RvIER & $7F) != 0`. `RvAnySlot` reflects only the
   slots enabled in `RvSEnb`.
2. **Built-in video VBL.** The video card asserts `RvIRQ0` (slot 0) once
   per frame; the boot ROM polls `RvSInt` bit 6 for it during video init
   and the OS VBL manager runs off it. `RvSInt` reads clear the `RvIRQ0`
   bit (clear-on-read VBL flag — one pulse per frame); NuBus slot bits
   (0-5) are level and not cleared on read.
3. **Soft power-off.** Writing 0 to `RvPowerOff` (`RvDataB` bit 2) fires
   the machine's power-off callback (the IIci stops the scheduler). An
   IIcx-style arm/debounce ignores the bit-low state the ROM leaves
   before the OS first drives it.
4. **Depth changes.** Writing the `RvMonP` depth field (bits 0-2) fires
   a mode callback so the video card reshapes `display_t`.
5. **Monitor sense.** The strap `rbv_init` latches into `RvMonP` bits 3-5
   is the code the bus seats the video card with, and the card's raster
   follows it (see "Monitor sense" below).

## Monitor sense

The monitor on the built-in port is chosen in the boot document, as on
any display device: `displays.builtin.monitor` (`catalog.profile` lists
the choices under `displays.builtin.monitors`). The build resolves it to
the 3-bit sense code, which both the RBV (`RvMonP`) and the video card
read; the `video_sense=` debug override sets a raw code instead. The
codes modelled (hardware reference [rbv.md](../../../reference/machines/mdu/rbv.md), monitor sensing §3.5):

| Sense | Monitor id | Raster | IIci (RBV) | IIsi (V8) |
|---|---|---|---|---|
| `110` (6) | `13in_rgb` (default) | 640×480, 1/2/4/8 bpp | yes | yes |
| `001` (1) | `15in_portrait` | 640×870, 1/2/4 bpp (no 8 bpp, Table 12-3) | yes | yes |
| `010` (2) | `12in_rgb` | 512×384, 1/2/4/8 bpp | reserved — halted | yes |
| `000` `011` `100` `101` | — | halted | halted | halted |
| `111` (7) | `none` | halted | halted | halted |

The decode is the card kind's monitor list: `builtin_rbv_video` (IIci)
lists the first two rows, `builtin_v8_video` (IIsi) all three, and a code
with no row there halts video. The geometry comes from
`display_timing_for_sense` (`display_timing.h`); stride is width × depth.
The raster is a power-up strap, fixed for the life of the machine (a
checkpoint restore re-seats the same code). A halted RBV presents a black
640×480 stub (`display_set_scanout` with no buffer) and raises no `RvIRQ0`
VBL, since it drives no sync. Measured: the IIci ROM drives the Portrait
at 640×870 (`ScreenRow` 80 at 1 bpp) and the IIsi ROM the 12" RGB at
512×384 (`ScreenRow` 64), both to the no-disk "?" screen
(`suite-iici` row `iici-portrait`, `suite-iisi` row `iisi-12in-rgb`).
Not modelled: the V8's printed quirks of ignoring monitor ID bit 1 and
reading `011` as VGA, and startup-mode (`displays.builtin.mode`) records
for any monitor but the 13" RGB.

The chip-test register, the genuine NuBus transfer-mode pins, the
parity-error generation, and the external-cache side effects are
accept-and-log in v1.

## Object / wiring surface

```c
rbv_t *rbv_init(rbv_variant_t variant, uint8_t sense3, checkpoint_t *cp); // RBV_VARIANT_IICI; sense3: the monitor strap (6 = 13" RGB, 7 = none)
const memory_interface_t *rbv_get_memory_interface(rbv_t *rbv);
void rbv_set_irq_callback(rbv_t *rbv, void (*cb)(void *, bool), void *ctx);   // -> IPL 2
void rbv_set_power_off_callback(rbv_t *rbv, void (*cb)(void *), void *ctx);
void rbv_set_blank_callback(rbv_t *rbv, void (*cb)(void *, bool video_off), void *ctx); // RvVIDOff, on change
void rbv_set_mode_callback(rbv_t *rbv, void (*cb)(void *, int depth), void *ctx);
void rbv_assert_slot_irq(rbv_t *rbv, int slot);  // slot 0 = built-in video
void rbv_clear_slot_irq(rbv_t *rbv, int slot);
void rbv_set_scsi_irq(rbv_t *rbv, bool active);  // RvSCSIRQ
void rbv_set_scsi_drq(rbv_t *rbv, bool active);  // RvSCSIDRQ
```

The `RBV_VARIANT_V8_IISI` superset (register file identical in size and
layout; a few bit names and the companion VDAC — Bt450 vs Bt478 — differ)
is carried by the IIsi ([iisi.c](../../../../src/machines/mdu/iisi.c));
the variant gates naming/inspection and any V8-only side effects.

## See also

- [docs/internals/machines/mdu/iici.md](iici.md) — the machine that uses the RBV.