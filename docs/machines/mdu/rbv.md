# RBV — RAM-Based Video chip

The **RBV** ("RAM-Based Video", Apple part 344S1019) is the combined
video-control + interrupt-aggregation ASIC used by the Macintosh IIci
(and, in its *V8* variant, the IIsi / LC family). In Granny Smith it is
implemented as a flat peripheral module:
[src/core/peripherals/rbv.c](../src/core/peripherals/rbv.c) /
[rbv.h](../src/core/peripherals/rbv.h).

On the IIci the RBV **replaces the VIA2** of the IIcx-family machines: it
lives at physical `$50F26000`, aggregates the slot / SCSI / sound
interrupts into a single 68030 **IPL 2** assertion, owns the
soft-power-off and external-cache control bits, and carries the built-in
video's monitor-sense + depth register. The framebuffer itself is owned
by the [builtin_rbv_video](../src/core/peripherals/nubus/cards/builtin_rbv_video.c)
NuBus pseudo-card and a Bt450 VDAC at `$50F24000`; the RBV only holds the
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
headers). The RBV memory interface decodes **both** the native small
offsets and these two aliases, so code written either way reaches the
same register.

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

The chip-test register, the genuine NuBus transfer-mode pins, the
parity-error generation, and the external-cache side effects are
accept-and-log in v1.

## Object / wiring surface

```c
rbv_t *rbv_init(rbv_variant_t variant, checkpoint_t *cp); // RBV_VARIANT_IICI
const memory_interface_t *rbv_get_memory_interface(rbv_t *rbv);
void rbv_set_irq_callback(rbv_t *rbv, void (*cb)(void *, bool), void *ctx);   // -> IPL 2
void rbv_set_power_off_callback(rbv_t *rbv, void (*cb)(void *), void *ctx);
void rbv_set_mode_callback(rbv_t *rbv, void (*cb)(void *, int depth), void *ctx);
void rbv_assert_slot_irq(rbv_t *rbv, int slot);  // slot 0 = built-in video
void rbv_clear_slot_irq(rbv_t *rbv, int slot);
void rbv_set_scsi_irq(rbv_t *rbv, bool active);  // RvSCSIRQ
void rbv_set_scsi_drq(rbv_t *rbv, bool active);  // RvSCSIDRQ
void rbv_set_monitor_sense(rbv_t *rbv, uint8_t sense3); // 6 = 13" RGB
```

The `RBV_VARIANT_V8_IIsi` superset (Apple-II mode, VRAM-vs-DRAM refresh
bit, Bt478 VDAC layout) is reserved for a future IIsi addition.

## See also

- [docs/machines/mdu/iici.md](iici.md) — the machine that uses the RBV.