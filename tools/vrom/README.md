# tools/vrom — generic NuBus video declaration ROM

Source and build tooling for the generic declaration ROM images the
emulator embeds so NuBus video cards work with no user-supplied ROM dump.
For what the ROM *is* and how the emulator uses it, see
[docs/core/peripherals/nubus_generic_vrom.md](../../docs/core/peripherals/nubus_generic_vrom.md).

## Files

```
Makefile          assembles the four personalities, stamps CRCs, embeds blobs
crc.py            Format-Block finaliser (Length / CRC / ByteLanes)
embed.py          built images → the committed gsvrom_blobs.h
gsvrom.s          top level: sResource directory, board sResource, records
gsvrom_equ.i      clean-room equates (no Apple AIncludes)
gsvrom_macros.i   OSLstEntry / DatLstEntry / sExec / VPBlock macros
gsvrom_init.s     shared PrimaryInit (+ SecondaryInit framework)
gsvrom_drvr.s     shared video DRVR: csCode dispatch, VBL ISR
ops_jmfb.s        JMFB personality      (equates, tables, leaf ops)
ops_boogie.s      Boogie (24AC) personality
ops_mdcgc.s       MDCGC (8•24 GC) personality
ops_se30.s        SE30 (built-in video) personality
```

The shared-core / personality split is described in the doc linked above;
in this tree the shared core is `gsvrom_init.s` + `gsvrom_drvr.s`, and each
`ops_*.s` is one personality selected at assembly time by a `--defsym`
symbol.

## Building

```
make -C tools/vrom
```

This assembles one image per personality, stamps each Format Block, and
regenerates the **committed** header
`src/core/peripherals/nubus/gsvrom_blobs.h` (the images embedded as C byte
arrays). Because that header is checked in, an ordinary emulator build needs
**no m68k toolchain** — you only run this target after editing the ROM
source, and commit the regenerated header alongside the change.

The assembler is GNU binutils targeting m68k: `m68k-linux-gnu-as` +
`objcopy -O binary`, from Ubuntu/Debian's `binutils-m68k-linux-gnu` package
(2.42 is the verified baseline; the devcontainer image ships it).  Any
m68k-targeted binutils works because the output is a raw binary — set
`M68K_AS=`/`M68K_OBJCOPY=` for e.g. an `m68k-elf` cross build on macOS.
The source is gas syntax in `.altmacro` mode; note the port rules baked
into `gsvrom_macros.i` (packed list entries use `+`, never `|` — gas
silently mis-folds a masked forward difference OR'd with a constant —
and strings use `.ascii`/`.asciz` because `dc.b` silently drops string
operands).

## Image layout

Each image is a dense 4-lane chip (`byteLanes $0F`) and is left at its
assembled size — not padded to a power of two. `crc.py` sets the
Format-Block `Length` and `CRC` to cover exactly the assembled content
(like the real 8•24 ROM, whose `Length` is `$4000` inside a 32 KB chip).
The emulator's declaration-ROM loader tail-places the image at the top of
the card's slot window, so only the meaningful bytes need to exist.

## Adding a personality

Support for another card model is a new `ops_<name>.s` (register equates,
the card's data tables, and its ~8 leaf routines), a personality branch in
`gsvrom.s` selecting it, and a build entry in the `Makefile`. The shared
core is not touched. See an existing `ops_*.s` for the exact set of symbols
a personality must define.
