# PCI expansion ROMs (PROMs)

`src/core/memory/prom.{c,h}`

A PCI expansion ROM is the firmware a card carries on board: a `$55AA`
signature, a PCI Data Structure describing the image, and — on a Macintosh
card — an IEEE 1275 **FCode** program that Open Firmware executes at probe
time to build the card's device-tree node, plus usually a PowerPC **ndrv**
published as `driver,AAPL,MacOS,PowerPC` so Mac OS can drive the card with
nothing installed from disk.

The emulator does not interpret any of it. The card's ROM is served to the
guest through the expansion-ROM BAR at config `$30`, and the **guest's own
firmware** runs it. That is the whole point of the FCode path: what the
device tree says about a card is what the card said about itself.

## Why this is a sibling of `vrom.c`, not a generalisation of it

Both modules answer the same question — "which file provides this card?" —
with the same architecture: **a path is a HANDLE, not a fact.** Core may
open a path it was handed and identify the bytes; it never fabricates a
path and never interprets a filename. The platform, which owns the
filesystem, enumerates candidates and *offers* them; core identifies each
by content and matches among the offers.

What they do not share is everything else. A vROM is identified by a NuBus
Format-Block CRC in its **trailing** bytes and comes in two fixed sizes. A
PROM is identified from a PCI Data Structure near its **head**, spans five
size classes, and has to tell an Open Firmware image apart from an x86 VGA
BIOS. The validation gates, the size classes and the identity spans
genuinely differ, so they are written out twice rather than parameterised
into one thing that serves neither well.

## Identity

The key is the **CRC-32 of the whole chip image** — the same kind of
intrinsic content identity `rom.c` takes from a main ROM's checksum word
and `vrom.c` from the Format-Block CRC. No emulator-invented hash, and no
filename ever enters the comparison.

## The gates

All of these must pass before the CRC is trusted. An unrecognised blob is
**dropped with a log**, never guessed at:

1. the file is a power of two, 2 KB … 256 KB (a plausible chip);
2. bytes `$00-$01` are `55 AA`;
3. the little-endian halfword at `$18` points inside the image at a `PCIR`
   signature;
4. the PCI Data Structure's **code type is `$01`** (Open Firmware);
5. the byte at the FCode offset is a `start0`/`start1`/`start2`/`start4`
   token (`$F0`–`$F3`);
6. the CRC-32 matches a `PROM_CATALOG` row.

Gate 4 is reported separately from "this is not a ROM", because a code-type-0
x86 option ROM is the **predictable user error** — "I flashed the ROM off a
PC Mach64" — and deserves to be told what it actually has:

```
prom.identify → { "recognised": false,
                  "reason": "an expansion ROM, but its code type is not Open
                             Firmware (a PC/x86 option ROM cannot drive a
                             Macintosh card)", ... }
```

A structurally valid Open Firmware ROM that simply is not catalogued reports
its vendor/device ids and CRC, so a future catalog row (or the user) can
identify it.

## The catalog

One row per known dump: CRC-32, chip size, the **pci card-kind id** the blob
provides, and a `preferred` bit picking the default where a card has several
dumps. Adding a card ROM is one row.

```c
{0x437584E0u, 0x8000, "mach64_gx", true },   // 113-32900-104 (and -004)
{0x8C68216Eu, 0x8000, "mach64_gx", false},   // 113-32900-101
```

## Pick order

`prom_offer_find()` yields candidates in this order, all content-based:

1. the explicit `machine.boot prom=` pick;
2. catalog rows carrying `preferred`;
3. the remaining catalog rows, in order.

## Serving the ROM to the guest

A card factory calls `prom_load_card()`, hangs the buffer on
`pci_device_t.rom` / `.rom_size`, declares `rom_size` in its
`pci_config_decl_t`, and registers a backing on `PCI_ROM_BAR_INDEX`:

```c
pci_bar_backing_iface(dev, PCI_ROM_BAR_INDEX, &priv->rom_if, priv);
```

`config_space.c` already gates the ROM BAR on both its own enable bit and
`PCI_CMD_MEM_SPACE`, and `pci_device_regions_changed()` maps it wherever the
firmware assigns it. The declared `rom_size` is the **physical chip size**,
which also covers a shorter declared image; reads past the image return
`$FF`.

The ROM image is deliberately **not checkpointed**: it is provisioned
content, re-resolved from the offer registry on restore, exactly as vROMs
are.

## Boot validation

A card kind that sets `requires_prom` and is **explicitly picked** by the
user fails the boot when nothing resolves, before the running machine is
touched:

```
machine.boot: card 'mach64_gx' (PCI slot 1) needs a PCI expansion ROM but no
offered .prom file provides it
```

The asymmetry is deliberate and mirrors the vROM rule: only *explicit*
picks fail. A slot resolving its own declared default degrades to an empty
slot with a log, so a machine whose default card has no ROM offered still
boots.

## Where the files come from

* **headless** — `offer_sibling_proms()` enumerates the directory of the
  ROM file it was given and offers every `*.prom`. A test script's
  `rom="${$ROM}"` therefore makes the card ROMs discoverable with no path
  knowledge anywhere in core.
* **web2** — a dropped file is probed against the media types in order
  (`rom`, `vrom`, `prom`, `fd`, `cdrom`, `hd`; `app/web2/src/bus/upload.ts`),
  stored under `/opfs/images/prom/` named by its CRC, and offered through
  `prom.offer(path)` — the same hook `vrom.offer` provides, so a file
  uploaded mid-session is visible to the next boot without a reload.  vROM
  and PROM cannot claim each other's files even though both are commonly
  32 KB: a vROM is keyed on a Format-Block CRC in its trailing bytes, a
  PROM on a PCI Data Structure near its head.

  The New Machine dialog's Display picker unions the machine's built-in
  video, its NuBus video cards, and every **display-class PCI card** whose
  expansion ROM is present; picking one sends `pci_card=` and `prom=` in
  the boot document.  A machine with no built-in video and no NuBus video
  slots — the Power Macintosh 9500 is the first — says so explicitly
  ("requires a display card in a PCI slot") when no `.prom` has been
  uploaded, rather than offering an empty picker.

Fixtures live in `gs-test-data`'s flat `roms/` directory under the naming
grammar in `scripts/rom_naming.py`:

```
<card-id, _ -> ->[-<rev>]-<crc8>.prom     e.g. mach64-gx-104-437584e0.prom
```

## Object surface

`machine.prom` (beside `machine.vrom`):

| Member | Meaning |
|---|---|
| `offer(path)` | register a candidate; `true` iff recognised |
| `identify(path)` | `{recognised, card_id?, compatible?, vendor_id?, device_id?, size, crc, reason?}` |

## See also

- `docs/core/peripherals/pci.md` — the bus, the config header, region backing
- `docs/core/peripherals/nubus_vrom.md` — the declaration-ROM sibling
- `scripts/fcode/detok.py` — detokenizes an FCode image, so what a card's ROM
  *does* can be read before its device model is written
