# Apple Macintosh Display Card 8•24

Apple's 1990 NuBus display card built on the **Jackson / JMFB / Elmer** ASIC.
Implemented in
[`src/core/peripherals/nubus/cards/jmfb.c`](../../../../../src/core/peripherals/nubus/cards/jmfb.c)
(the source and its whole register set keep the ASIC codename `JMFB`, per Apple's
own engineering naming). Catalogued in [`video.md`](../../video.md) §2.2; vROM
byte-layout reference in [`nubus_vrom.md`](../../nubus_vrom.md).

> **Naming:** this file is named by the *product* (8•24) to sit parallel with
> [`display_card_24ac.md`](display_card_24ac.md); the implementation unit is
> `jmfb.c` (`JMFBCSR`, `JMFBVideoBase`, …). Apple video parts are known by ASIC
> codename; the third-party 24AC has none, only a brand — hence the two docs read
> by product name but point at differently-named sources.

| | |
|---|---|
| **Card kind** | `mdc_8_24` |
| **ASIC** | Jackson / JMFB / Elmer (successor to Toby/TFB) |
| **vROM** | `Apple-341-0868.vrom` (ROM `341-0868`, Rev B) |
| **Slot** | `$9` **default** video card on IIcx, IIx, IIfx |
| **VRAM** | 2 MB (`defMinorLengthB`) |
| **decl-ROM** | 32 KB chip → 128 KB bus footprint (byteLanes `$78`), at slot `0xFE0000` |
| **Depths** | up to 24 bpp per the vROM mode tables (modelled monitors list 1/2/4/8) |
| **Accelerator** | none (contrast the [24AC](display_card_24ac.md)) |

## 1. What is modelled

Minimum-viable (proposal `proposal-machine-iicx-iix.md` §3.2.5): enough to boot
System 7 to a colour desktop and let the Monitors control panel switch depth.

- The card factory loads `Apple-341-0868.vrom` and registers VRAM, the
  declaration ROM, and the register window on the bus.
- One I/O dispatcher handles all four register blocks (JMFB / Stopwatch / CLUT /
  Endeavor). The registers the boot path depends on are **modelled**; everything
  else is **accept-and-log** so an OS write that should be a no-op never
  bus-errors.
- **Sense lines** satisfy the JMFB `PrimaryInit` read, so the OS picks up the
  user's `monitor=` choice (`mdc_8_24_monitors[]`, matched by name/`spID`).
- **CLUT** writes feed `display.clut` (`clut_dirty`); depth changes via
  `CLUTPBCR` feed `display.format` (`shape_dirty`).
- **VideoBase / RowWords** are modelled; the framebuffer base lives in VRAM
  (units of 32 bytes), unlike the 24AC's driver-private base.
- Mode-table parsing inside the vROM is left to the System 7 driver — we present
  the bytes; it walks them.

## 2. Register map

Four blocks live in a `0x400`-byte window based at slot offset `0x200000`
(add `nubus_slot_base($9)`). Offsets below are within each block.

**JMFB block** (base `0x200000`):

| Off | Register | Notes |
|---|---|---|
| `0x00` | `JMFBCSR` | Control & Status; monitor **sense bits live in bits 9-11** (mask `0xF1FF`) |
| `0x04` | `JMFBLSR` | Load / Sync |
| `0x08` | `JMFBVideoBase` | framebuffer base in VRAM, units of 32 bytes |
| `0x0C` | `JMFBRowWords` | stride encoding (depth-dependent) |

**Stopwatch block:**

| Off | Register | Notes |
|---|---|---|
| `0x48` | `SWClrVInt` | clear pending VBL interrupt (write-1-to-clear) |
| `0xC0` | `SWStatusReg` | Stopwatch status (VBL toggle state) |

**CLUT block** (base `0x200200`):

| Off | Register | Notes |
|---|---|---|
| `0x00` | `CLUTAddrReg` | RAMDAC palette index; also resets the R/G/B sub-counter |
| `0x04` | `CLUTDataReg` | palette data; 3 sequential writes load R, G, B |
| `0x08` | `CLUTPBCR` | Pixel Bus Control; bits 3-4 select depth on the RAMDAC |

VBL: clearing the `JMFBCSR` interrupt-enable bit makes the card raise a slot IRQ
on every VBL; setting it masks the VBL IRQ.

## 3. No acceleration engine

The 8•24 has no QuickDraw fill/copy accelerator — QuickDraw draws straight into
the linear framebuffer and the OS uses its software `CopyBits`/`ScrollRect`. (The
**8•24 GC** added an AMD Am29000 RISC for accelerated QuickDraw; that is a
separate driver lineage and is not modelled.) The card with a hardware engine in
this tree is the [24AC](display_card_24ac.md).

## 4. vROM identity

`machine.vrom.identify` keys off the declaration ROM's NuBus Format-Block CRC and
returns `card_id = "mdc_8_24"` with `compatible = [...]`. See
[`nubus_vrom.md`](../../nubus_vrom.md) and
[`src/core/memory/vrom.c`](../../../../../src/core/memory/vrom.c).

## 5. Tests

| Test | Coverage |
|---|---|
| `tests/integration/iicx-video-modes` | JMFB depth/mode switching across monitors |
| `tests/integration/vrom-identify` | CRC identity + `card_id` / `compatible` |
| IIcx / IIx / IIfx boot tests | exercised as the **default** slot-`$9` video card |

## 6. Provenance

Design contract: `proposal-machine-iicx-iix.md` §3.2.5 + `jmfb.h`. Historical
survey and the Toby→JMFB→DAFB lineage: [`video.md`](../../video.md) §2.
