# Cirrus Logic 54M30 (Apple Network Server on-board video)

The Apple Network Server's soldered-down display controller: a discrete PCI
SVGA part on Bandit 1 at IDSEL 15, with a standard VGA connector, rather than
a framebuffer hung off the memory controller. Modelled in
[`src/core/peripherals/pci/cards/cirrus54m30.c`](../../../../../src/core/peripherals/pci/cards/cirrus54m30.c);
the machine side (the boot path, what Open Firmware programs, the legacy I/O
decode) is in [`docs/machines/tnt/tnt.md`](../../../../machines/tnt/tnt.md).

| | |
|---|---|
| **Card kind** | `cirrus_54m30`, `PCI_ATTACH_BUILTIN` (only where a machine's slot table names it) |
| **PCI ID** | `1013:00A0`, class `$030000`, revision `$00` |
| **BARs** | BAR0: 16 MB prefetchable memory, the display-memory aperture. BAR1: 512-byte I/O, the relocatable VGA range |
| **Fixed decode** | the legacy VGA block `$3B0`-`$3DF` in I/O space, a strapped claim rather than a BAR |
| **Memory** | 1 MB of DRAM at the bottom of the 16 MB aperture; the rest reads zero and ignores writes |
| **ROM** | none |
| **Interrupt** | none (interrupt pin 0) |
| **Depth** | 8 bpp only |

The part is a GD5430/5440-family Alpine die; the register semantics below are
the VGA ones plus the Alpine extensions (Cirrus Logic, *Alpine VGA Family
CL-GD543X/4X Technical Reference Manual*, 4th ed., sections 4.14-4.20).

## Registers the model decodes

Both I/O windows (BAR1, indexed by the port's low byte, and the legacy block)
reach one register file (`io_read8` / `io_write8`, lines 251-355):

- **Sequencer** `$3C4`/`$3C5`, **CRTC** `$3D4`/`$3D5`, **graphics** `$3CE`/`$3CF`:
  index/data pairs, store and read back. A data write re-derives the mode.
- **Attribute** `$3C0` (write, index and data alternating) and `$3C1` (read).
  Reading Input Status 1 resets the index/data flip-flop.
- **DAC** `$3C8` (write index), `$3C7` (read index), `$3C9` (data): R, G, B per
  entry with auto-advance. Values are six bits; the palette handed to the
  renderer replicates the top two bits into the bottom, so `$3F` becomes `$FF`
  (line 339).
- **Input Status 1** `$3BA`/`$3DA`: not store-and-readback. Bits 3 (vertical
  retrace) and 0 (display enable inactive) are set for the last 1/14 of each
  1/60 s frame of emulated time, from the scheduler's cycle count
  (`status1_value`, line 223), so a wait-for-retrace loop ends and a run stays
  deterministic.

Every other port stores the byte and reads it back.

## How the mode is derived

The card has no mode register. `c54m30_update` (line 402) runs after every
sequencer, CRTC or graphics data write and computes:

| Quantity | Source |
|---|---|
| depth | SR07 bit 0 enables the extended modes; bits 3:1 = `000` is 8 bpp (`c54m30_bpp`, line 383) |
| width | (CR01 + 1) x 8, or x 9 when SR01 bit 0 is clear |
| height | CR12, plus CR07 bit 1 as bit 8 and CR07 bit 6 as bit 9, plus 1 |
| stride | CR13 x 8 bytes |
| start | CR0C:CR0D, plus CR1B bit 0 as bit 16 and CR1B bits 3:2 as bits 18:17, in doublewords (x 4 bytes) |

Open Firmware's 640x480 sequence (SR07 = `$F1`, CR01 = `$4F`, CR12 = `$DF` with
CR07 bit 1, CR13 = `$50`) therefore gives 640x480, 8 bpp, 640 bytes per line.

What the model does with a register state it cannot present (lines 403-446):

- **Not 8 bpp** (standard VGA, or a 16/24/32 bpp extended mode): nothing
  changes; the last good mode stays. Before any good mode there is none, and
  the display op returns NULL so the machine's primary-display search moves on.
- **A half-programmed CRTC** (zero or oversized width or height, or a stride
  shorter than a line): also ignored until the rest arrives.
- **A start address that runs the raster past 1 MB**: scanning restarts at
  offset 0.
- **A raster larger than 1 MB even from offset 0** (a 2040-byte stride at
  768 lines, say): `display_set_scanout` refuses it and, with no blank buffer,
  the display op returns NULL.

A checkpoint stores the register files, the palette and all of display memory;
restore rebuilds the palette and the scanout from them (line 574).

## What it does not implement

- **No expansion ROM.** The Open Firmware node is built by `54m30-config` in
  the main ROM, so `rom_size` is 0 and the ROM BAR reads zero (line 99).
- **No interrupt.** Apple states the part has no interrupt line; the
  declaration's interrupt pin is 0 (line 90), so no Grand Central external
  interrupt is allocated to it.
- **8 bpp only.** Apple: the controller "implements only a little-endian window
  into the packed-pixel frame buffer, hence Big Endian operating systems are
  limited to 8 bits per pixel." At one byte per pixel byte order does not
  matter, so `PIXEL_8BPP` is correct as it stands; deeper modes would need a
  little-endian framebuffer format in the display layer.
- **No acceleration.** Period software drove the part as a plain framebuffer.

## Tests

[`tests/unit/suites/cirrus54m30/`](../../../../../tests/unit/suites/cirrus54m30/test.c)
(`make -C tests/unit test-cirrus54m30`) links the card and the real
config-space code and checks the mode derivation above (640x480, 1024x768,
nine-dot clocks, the start address and its extension, and every "no valid
mode" case), BAR sizing, the absent ROM and interrupt, the port behaviour and
a checkpoint round-trip. The boot-level tests are the Network Server rows of
`suite-ans`.
