# CPU ROMs: layout, checksums and identity

## 1. Overview

A Macintosh or Lisa CPU ROM carries its own checksum, and the ROM's startup
code verifies it before anything else. This document describes the three
layouts the emulator knows — the 68k Macintosh ROM, the 4 MiB Old World
PowerPC ROM, and the Lisa / Macintosh XL boot ROM — how each checksum is
computed, and why a ROM's identity is the checksum it carries.
[§10](#10-rom-provisioning) describes how the emulator uses that identity
to obtain, verify and store ROMs.

A ROM does not always determine the machine: one image often serves several
models, which tell themselves apart at run time from hardware straps (§6).
The emulator's table of known ROMs, with the models each one boots, is
`src/core/memory/rom_table.c`.

## 2. The header

68k Macintosh ROMs and the Old World PowerPC ROMs start the same way. There
is no ASCII magic number.

| Offset | Size | Contents |
|---|---|---|
| `$00` | 4 | The **checksum** (§3). On a 68k machine the ROM is mapped at address 0 at reset, so the processor also loads this longword as its initial stack pointer; the startup code sets its own stack at once. |
| `$04` | 4 | The **reset PC** (e.g. `$0040002A` on a Plus, `$4080002A` on the IIci). |
| `$08` | 2 | The **ROM version**, one value per ROM family (below). |
| `$0A` | 2 | The start of code — a `BRA` (`$6000`) on the Plus, a `JMP` (`$4EFA`) from the Universal ROM on — not a data field. |
| `$12` | 2 | The **release** within the family, on `$067C` and `$077D` ROMs (`$10F1` for the IIci, `$28F1`/`$28F2` for the two TNT ROMs). |

| Version | ROMs |
|---|---|
| `$0069` | Macintosh 128K / 512K (64 KB) |
| `$0075` | Macintosh Plus and 512Ke (128 KB) |
| `$0178` | Macintosh II and the Universal IIx/IIcx/SE/30 ROM (256 KB) |
| `$0276` | Macintosh SE, SE FDHD and Classic |
| `$037A` | Macintosh Portable and PowerBook 100 |
| `$067C` | The 32-bit clean 68k ROMs of 1989–1995: IIci, IIfx, IIsi, LC, Quadra 700/900 and 950, and later |
| `$077D` | The Quadra 840AV/660AV ROM, the PowerBook 190 ROM, and every Old World PowerPC ROM |

The version and release name a *build*, not a unique image: the production
Apple Network Server ROM carries the same version and release (`$077D`,
`$28F2`) as the Power Macintosh 9500 v2 ROM. A ROM is never identified by
its version.

The Lisa boot ROM does not follow this layout (§5; the machine's own page is
[lisa.md](../machines/lisa/lisa.md) §16).

## 3. The 68k checksum

The checksum is a 32-bit sum of the ROM's 16-bit big-endian words, from
offset 4 to the end of the image, wrapping on overflow. The stored value at
`$00` is left out; an intact ROM's sum equals it.

```c
uint32_t sum = 0;
for (size_t i = 4; i + 1 < size; i += 2)
    sum += (data[i] << 8) | data[i + 1];   // intact: sum == BE32(data + 0)
```

The startup code runs the same sum over the ROM as one of its first
diagnostics, and a mismatch stops the boot with a Sad Mac (a ROM test
failure).

The sum covers the whole image on every 68k ROM we know except one: the
Macintosh Classic's sum stops at 256 KB, where its built-in ROM disk (§8)
starts. The ROM table records that as a per-row span.

A word sum is blind to the order of the words it adds, so two dumps whose
words are merely rearranged — the result of crossed address lines in a
reader — carry the same, verifying sum.

## 4. The Old World PowerPC checksums

The 4 MiB ROMs of the NuBus and PCI Power Macintoshes, the Network Server
and their relatives are a 3 MiB 68k half — the Toolbox, with the header and
checksum of §3 — followed by a 1 MiB PowerPC section (hardware
initialisation, the nanokernel, the 68k emulator). The header sum covers
only `[4, $300000)`, the 68k half.

The PowerPC section carries a second record. The longword at `$300080` is
the offset from `$300000` of a 40-byte record Apple's source calls
`NKConfigurationInfo`; the emulator requires the offset to be a multiple of
8 and the record to lie inside the PowerPC megabyte. Its first 40 bytes are

| Offset in the record | Size | Field |
|---|---|---|
| `$00` | 8 × 4 | `ROMCheckSumByte0..7`: for each byte lane 0–7 (the byte's offset within its doubleword, i.e. its ROM chip), the 32-bit sum of every byte in that lane |
| `$20` | 8 | `ROMCheckSum64`: the 64-bit sum of the image's big-endian doublewords |

Both cover the **entire** image except these 40 bytes, so they see what the
header sum cannot. Apple defined the record in internal source and never
published it; the values below were recomputed from the dumps and verify.

| ROM | Header sum | Record at | `ROMCheckSum64` |
|---|---|---|---|
| Power Macintosh 6100/7100/8100 | `$9FEB69B3` | `$30C000` | `$DEDC602CFC3B9221` |
| Power Macintosh 7500/8500/9500 v1 | `$96CD923D` | `$30D000` | `$C241CD82BF90797A` |
| Apple Network Server, Open Firmware 1.1.22 | `$962F6C13` | `$30D000` | `$D540B3DD5BCF9CAA` |
| Apple Network Server, 2.0 prototype | `$49B2BE8F` | `$30D000` | `$5F2AEEB25507B2CB` |

The header sum alone is ambiguous here. Three Network Server ROMs (Open
Firmware 1.1.20.1, 1.1.22 and 2.26NT) share `$962F6C13`, and the 2.26B6 ROM
shares `$9630C68B` with the Power Macintosh 9500 v2 ROM: the ROMs differ
only in the PowerPC section. And damage in the last megabyte leaves the
header sum intact. The 64-bit sum resolves both, and when it fails the lane
sums say which chip is bad.

The same record is present in the "Mac OS ROM" file a New World Macintosh
loads from its System Folder. The Bandai Pippin ROMs have it too, but their
sums cover some other range; the ROM table verifies only their 68k half.

## 5. The Lisa and Macintosh XL boot ROM

The 16 KB boot ROM is two 8 KB byte-slice chips, one holding the high byte
of every word and one the low byte, interleaved into one image. It has no
Macintosh header:

| Offset | Contents |
|---|---|
| `$0000` | The reset SSP, `$00000480`, on every revision |
| `$3FFC` | The version: `$02` `'H'` for the Lisa 2 rev H ROM (`$0248`), `$0341` for the Macintosh XL "3A" ROM |
| `$3FFE` | The **check word** |

The ROM's first diagnostic, `ROMTST` in the Lisa Boot ROM source (rev 2H),
checks it. It adds each word from `$0000` up to `$3FFE` into a 16-bit
accumulator and rotates the accumulator left one bit after each addition
("to catch multiple bit errors"), then adds the check word; an intact ROM
leaves zero, and a failure hangs the machine before it can display
anything. The check word is chosen to make the sum zero, so it identifies
the ROM build. The check covers both chips, so a swapped high/low pair
fails it.

| ROM | Check word | Version |
|---|---|---|
| Lisa 2, rev H | `$3F7B` | `$0248` |
| Macintosh XL, "3A" | `$D905` | `$0341` |

## 6. One ROM, several machines

From the Macintosh II family on, Apple shipped one ROM image in several
models, and the ROM decides which machine it is running on from hardware
straps. The Universal 256 KB ROM (`$97221136`) serves the IIx, IIcx and
SE/30 and reads two port bits:

| Model | VIA1 PA6 | VIA2 PB3 | Gestalt machine ID |
|---|---|---|---|
| Macintosh IIx | 0 | 0 | 7 |
| Macintosh IIcx | 1 | 1 | 8 |
| Macintosh SE/30 | 1 | 0 | 9 |

(`src/machines/glue/iix.c`, `iicx.c`, `se30.c` drive these straps.) The
same holds later: the Power Macintosh 6100/7100/8100 share one ROM and are
told apart by a machine-ID register, and the 7500/8500/9500 by the Grand
Central BoxID register. So the ROM table lists every model a ROM boots, and
the caller picks the model.

The Universal ROM is also "32-bit dirty": it uses the top byte of
addresses for flags, so these machines need the MODE32 extension to run
the Memory Manager in 32-bit mode. The `$067C` ROMs, from the IIci on, are
32-bit clean.

## 7. The Macintosh Plus revisions

All three Plus ROMs are 128 KB, two 64 KB chips, with version `$0075`
([compact.md](../machines/compact/compact.md) §2.5), so
only the checksum tells them apart:

| Revision | Checksum | Name |
|---|---|---|
| 1 | `$4D1EEEE1` | "Lonely Hearts" |
| 2 | `$4D1EEAE1` | "Lonely Heifers" |
| 3 | `$4D1F8172` | "Loud Harmonicas" |

The later revisions fix SCSI problems of the first.

## 8. Inside the ROM

From the Macintosh Plus on, a ROM holds a resource map — `DRVR` drivers,
`CODE`, `FONT`, `CURS` and the rest — that the Resource Manager reads as the
ROM resource file. The Macintosh Classic's ROM also carries a bootable
System disk in its upper 256 KB (booted by holding Command-Option-X-O at
startup), which its header sum does not cover (§3).

## 9. Checking a dump

`machine.rom.identify(path)` answers for any file, with or without a
machine: its `kind`, its `id` (the stored checksum field(s): §3, §4, §5),
whether its own checksum verifies (`intact`, with a `reason` naming the
part that does not), and — when the ROM is known — its name and the models
it boots. From the command line, with a two-line script file:

```
$ cat identify.script
echo "${machine.rom.identify("/path/to/dump.rom")}"
quit
$ build/headless/gs-headless rom=<any bootable ROM> --no-prompt script=identify.script
```

A verifying checksum does not prove the bytes are in the right order (§3),
and a checksum is not a unique fingerprint of a file — two dumps can differ
where a checksum does not look (§4). What makes a ROM a *known* ROM is its
id matching a row of the ROM table, whose rows come from dumps verified
here and from published checksum lists.

## 10. ROM provisioning

This section describes how the emulator obtains the ROMs it runs: the CPU
ROM, NuBus declaration ROMs (vROMs), and PCI expansion ROMs (PROMs). One
rule covers all three. **A path is a handle, not a fact.** Core opens a
path it was given and identifies the bytes; it never builds a path of its
own and never reads meaning into a filename. The platform owns the
filesystem and decides which files core sees. Line numbers are as of this
writing; the function names are the stable reference.

### 10.1 Identity is content

| Kind | Identity | Gates before the identity is trusted | Catalog |
|---|---|---|---|
| CPU ROM | The ROM's own **stored checksum field(s)**, read verbatim (§10.1.1) | None beyond reading the file. The self-check (`intact`) is reported, and gates naming: a damaged dump keeps its id but is never stored by it (`rom_identify_data`, `src/core/memory/rom.c`) | `rom_table` (`src/core/memory/rom_table.c`): id → family name, compatible models, size, per-row exceptions |
| vROM (NuBus declaration ROM) | The Format Block **CRC**, read from the last 12 bytes of the chip image ([declaration-rom.md §2](../hardware/nubus/declaration-rom.md)) | Size 32 KB or 64 KB, and the `$5A932BC7` TestPattern (`vrom_identify_core`, `src/core/memory/vrom.c:113`) | `VROM_CATALOG` (`vrom.c:82`): CRC → card-kind id, plus a `preferred` bit |
| PROM (PCI expansion ROM) | **CRC-32 of the whole chip image** | Power-of-two size between 2 KB and 256 KB, `$55AA`, a `PCIR` structure, Open Firmware code type, and an FCode start token ([expansion-rom.md](../hardware/pci/expansion-rom.md#the-gates)) | `PROM_CATALOG` (`src/core/memory/prom.c`): CRC → card-kind id, plus `preferred` |

A CPU ROM does not choose the machine. The Universal ROM, for example,
lists `se30`, `iicx` and `iix`, so the caller names the model and the ROM
only has to be compatible with it (`rom.c`, file header).

#### 10.1.1 The CPU ROM id and self-check

Every family the emulator knows carries its own checksum. The id is those
stored field(s) as lowercase hex; `intact` is the same checksum recomputed
over the bytes (`identity_eval`, `rom.c`). The kind is a content rule, so
an unknown ROM gets an id and a verdict too:

| Kind | Rule | `id` | Self-check |
|---|---|---|---|
| `lisa` | 16 KB, first longword `$00000480` (the reset SSP) | the check word at `$3FFE`: `3f7b` | the boot ROM's own first diagnostic: add each word from `$0000` and rotate left one bit, up to `$3FFE`, then add the check word; an intact ROM leaves zero (Lisa Boot ROM source, rev 2H, `ROMTST`) |
| `ppc` | exactly 4 MiB | the header sum at `$00`, a dash, the ConfigInfo 64-bit sum: `9feb69b3-dedc602cfc3b9221` | the header sum over `[4, $300000)` (the 68k half) **and** the 64-bit sum of big-endian doublewords over the whole image except the 40-byte ConfigInfo record |
| `mac68k` | anything else with an even size of at least 8 bytes | the header sum at `$00`: `4d1f8172` | the header sum over `[4, size)` (§3) |

The Old World PowerPC ROMs keep, at `$300000 + BE32[$300080]`, a 40-byte
record Apple's source calls `NKConfigurationInfo`: eight 32-bit byte-lane
sums (`ROMCheckSumByte0..7`) followed by the 64-bit sum (`ROMCheckSum64`),
both over the entire image. Apple defined the record in internal source and
never published it. The header sum covers only the 68k half, so it cannot
tell apart two ROMs whose first 3 MiB agree — three Apple Network Server
ROMs share `962F6C13`, and the ANS 2.26B6 ROM shares `9630C68B` with the
Power Macintosh 9500 v2 ROM — nor see damage in the last megabyte. The
64-bit part of the id does both. When it fails, the reason names the byte
lanes whose recomputed sum differs, which narrows a bad dump to a chip.

Why a *stored* label and not a computed one: a dump either passes its
self-check, and then the recomputed sums equal the stored fields by
definition, or it fails, and then nothing is ever named by its id (the
browser refuses it). Under that gate the two schemes agree on everything
that is stored, and the stored label keeps the one thing a damaged dump
still knows — which ROM it was meant to be — so the refusal can say so.

Two known ROMs need a per-row exception, applied only when the id matches
their row: the Classic, whose header sum stops at 256 KB where its built-in
ROM disk starts (`checksum_span`), and the Bandai Pippin ROMs, whose
ConfigInfo sums cover some other range (`ROM_F_NO_SUM64`: only the 68k half
is verified).

#### 10.1.2 The ROM table

`rom_table.c` lists every CPU ROM the emulator knows about — about 135,
from images verified here, a published list of ROM checksum fields, and
published checksum values alone — each with the emulated models it boots.
An empty model list marks a real ROM for a machine that is not emulated:
`rom.identify` reports it `recognised` but not `supported`, `machine.boot`
and headless refuse it by name, and the browser refuses the upload by
name. Adding a machine model means adding its id to the compatible lists
of the ROMs it runs. The supported ROMs:

| Id | Compatible models |
|---|---|
| `4d1eeee1`, `4d1eeae1`, `4d1f8172` | `plus` |
| `97221136` | `se30`, `iicx`, `iix` |
| `4147dd77` | `iifx` |
| `368cadfe` | `iici` |
| `36b7fb6c` | `iisi` |
| `420dbff3` | `q700`, `q900` |
| `3dc27823` | `q950` |
| `5bf10fd1` | `q840av`, `q660av` |
| `9feb69b3-dedc602cfc3b9221` | `pm6100`, `pm7100`, `pm8100` |
| `9b7a3aad-ed510ac937721e25` (newer "Boot PDM 601 1.1" ROM) | `pm7100` |
| `96cd923d-c241cd82bf90797a` (v1), `9630c68b-4db4a42fea3b53b3` (v2) | `pm7500`, `pm8500`, `pm9500` |
| `962f6c13-c60da96de537f08a` (OF 1.1.20.1), `962f6c13-d540b3dd5bcf9caa` (OF 1.1.22), `962f6c13-50348b3d0126096b` (OF 2.26NT), `9630c68b-a71fb907dd180b8a` (OF 2.26B6), `49b2be8f-5f2aeeb25507b2cb` (2.0 prototype, Mac OS) | `ans500`, `ans700` |
| `3f7b` | `lisa` |
| `d905` | `macxl` |

The production Network Server ROM carries the same version fields as the
9500 v2 ROM, so a ROM is never identified by its version: only the full
id tells these apart.

A vROM that is not an Apple dump can still be recognised structurally: an
image produced by one of the emulator's own generic cards is identified by
its `granny-smith` VendorId and BoardId (`vrom.c:158-191`). The generic
kinds generate their declaration ROM when the card is built, and they never
consult the registry ([nubus_generic_vrom.md](../../internals/core/peripherals/nubus_generic_vrom.md)).

The identify surfaces answer from content alone. Each returns `V_ERROR` for
an unreadable path and `recognised: false` for a file that does not
identify:

| Method | Answer |
|---|---|
| `machine.rom.identify(path)` | `{recognised, supported, compatible, name, variant, size, kind, id, intact, reason}` (`rom_method_identify`, `rom.c`). `variant` is the table's short label that tells this ROM apart from other known ROMs booting the same model ("Win NT"), empty when no other known ROM shares a model with it. An unrecognised file still reports `kind`, `id`, `intact` and `reason`; `reason` is empty when intact and otherwise names what does not verify ("PowerPC section does not verify (byte lanes 2)") |
| `catalog.vroms.identify(path)` | `{recognised, card_id?, compatible?, size, crc}`, with `crc` as `0x` plus 8 lowercase hex digits (`vrom.c:320`) |
| `catalog.proms.identify(path)` | `{recognised, card_id?, compatible?, vendor_id?, device_id?, size, crc, reason?}` (`prom.c`, `prom_method_identify`) |

### 10.2 What `rom=` resolves

`machine.boot rom=` takes a **filesystem path**. Nothing searches for it
or looks it up in a registry. A relative path resolves against the
process's working directory. The ROM is a construction argument: before
the running machine is touched, the file — or, in the two-chip Lisa form
(`rom2=`), the two chips interleaved — is read and must identify through
`rom_table`, must boot at least one emulated model, must list the
requested model as compatible, and must be exactly the model's ROM size;
anything else rejects the boot (`boot_rom_read`, `src/machines/machine.c`).
The bytes travel in `machine_build_opts_t.rom`, `memory_map_init` creates
the ROM region filled, and the CPU starts from its reset vector by the
same path every reset takes. A damaged ROM (not `intact`) still boots,
with a warning naming the part that does not verify: research on damaged
or hand-edited images is a legitimate headless use. The path and the id
go into the built-from record as `machine.config.rom` and
`machine.config.rom_id`.

A running machine's ROM is never swapped: a different ROM is a new
`machine.boot`.

`machine.rom.id`, `machine.rom.intact` and `machine.rom.name` describe the
loaded ROM by running the same identification over the ROM region, so
`rom.id` always equals what `rom.identify` said about the file and what
`machine.config.rom_id` records.

The Lisa two-chip loader (`rom_load_lisa_pair`) picks the high/low chip
orientation whose interleave passes the boot ROM's own self-check; the check
covers both chips, so a swapped pair never passes it.

### 10.3 Card ROMs: the offer registry

Declaration ROMs and expansion ROMs are not named in the boot document by
default. The platform **offers** candidate files, and a card factory asks
for the ROM of its card kind unless its slot names one. The registry (`src/core/memory/offer_registry.c`)
has two instances, `vrom.c` and `prom.c`, with the same behaviour:

- **Registration by content.** `offer_registry_add` runs the kind's
  identifier. A file that is not recognised is dropped with a log, since
  strays are expected when a whole directory is offered. The registry
  keeps one entry per content id, and offering the same bytes again
  refreshes the stored path (`offer_registry.c:19-70`).
- **Pick order.** Catalog rows marked `preferred`, then the remaining
  catalog rows in order. No filename ever enters the comparison
  (`offer_registry_find`). A card loader tries the candidates in that order
  and takes the first that lays out cleanly (`declrom_load_vrom_card`,
  `src/core/peripherals/nubus/declrom.c`; `prom_load_card`). Its choice is
  recorded in `machine.config.vroms`.
- **A slot's own ROM.** A boot document may name the file for a slot:
  `machine.boot slots="9=824gc,rom=<file>"`, or `vrom=`/`prom=` as sugar for
  every slot whose card the file provides. The file is identified and
  checked against the slot's card before teardown, and it is handed to that
  card's constructor as an argument: the loader takes it instead of the
  catalog (`declrom_load_vrom_card(card_id, rom, ...)`,
  `prom_load_card(card_id, rom, ...)`). A boot never writes the registry,
  so a later boot that names no file resolves by catalog order again.
- **Strict resolution.** A card the document names that finds no ROM — its
  slot's file or an offer — fails the boot before teardown
  (`vrom_card_resolvable(card, rom)` / `prom_card_resolvable(card, rom)`,
  checked by `machine_slots_resolve`). A default card degrades to an empty
  slot, and the SE/30's onboard video synthesises a fallback ROM
  (`src/machines/glue/builtin_se30_video.c`). See
  [object-model.md, Boot arguments](../../internals/core/object/object-model.md#boot-arguments).
- **Lifetime.** The registries are process-global. Offers survive
  `machine.boot`, `machine.restart` and `checkpoint.load`, and are dropped
  only by `vrom_delete`/`prom_delete`. The card ROMs themselves are not
  checkpointed; on restore each slot's is resolved again from the record's
  slot entry (its named file) or the registry (`system_restore`).
- **Hooks.** `catalog.vroms.offer(path)` and `catalog.proms.offer(path)`
  register one file and return `true` only if it was recognised.

### 10.4 Where the files come from

| | Headless (`src/platform/headless/headless_main.c`) | Browser (`app/web2`, `src/platform/wasm/em_main.c`) |
|---|---|---|
| CPU ROM | The `rom=` argument on the command line, which must identify. `model=` picks among the compatible models and otherwise defaults to the first (`headless_main.c:1297-1326`). A script names further ROMs by path in its own `machine.boot rom=`. The integration runner exports the startup path as `$ROM` (`scripts/run-integration-test.sh:154-164`). | Stored in OPFS as `/opfs/images/rom/<id>`, named by the content id `rom.identify` reports, and only when the ROM is `intact` and `supported`: a damaged dump or a ROM of an unemulated machine is refused with a message saying which (`app/web2/src/lib/media.ts`, the `rom` descriptor). The configuration dialog lists that directory and identifies each file, offering one Machine Model entry per model/ROM pair, labelled "<model> (<variant>)" whenever the ROM has a `variant` (that is, whenever other known ROMs boot the same model, stored or not). A dropped ROM boots its first compatible model if no machine is running (`maybeBootFromRom`, `app/web2/src/bus/upload.ts:440`). A URL `?rom=` is fetched, stored, and booted, using the URL's `model=` when it is compatible (`app/web2/src/bus/urlMedia.ts:119-134`). |
| vROM | Before the startup boot, every `*.vrom` in the directory of the command-line ROM is offered (`offer_sibling_card_roms`, `headless_main.c`), and `machine_boot_apply` repeats the walk for the directory of every ROM a script boots, through `platform_offer_sibling_card_roms` (platform.h; the browser's implementation is a no-op) — a `machine.boot rom=<elsewhere>` used to find none of the card ROMs beside it (#187). | Every file in `/opfs/images/vrom/` is offered at startup, whatever its name (`em_main.c:724`). An upload is stored as `<crc>`, 8 lowercase hex digits (`media.ts:180`), and offered at once through `catalog.vroms.offer` (`upload.ts:397`). |
| PROM | The same offer pass for `*.prom` (`headless_main.c:946`). | The same, from `/opfs/images/prom/` (`em_main.c:729`; `upload.ts:401`). |

In the browser, a dropped file is classified by trying the
identifiers in the order `rom`, `vrom`, `prom`, `fd`, `cdrom`, `hd` (`upload.ts`, `probeStaged`).
A descriptor that recognises the file but refuses it (`reject`) ends the
probe with that message, so a refused ROM is never stored as a disk image.
vROM and PROM cannot claim each other's files, because they identify from
opposite ends of the image.

Headless offers card ROMs once, from the directory of the **command-line**
ROM. `machine_boot_apply` never offers anything, so a script that calls
`machine.boot rom=` with a ROM in another directory does not make that
directory's `*.vrom`/`*.prom` visible. It has to offer them
(`catalog.vroms.offer`/`catalog.proms.offer`) or name one with
`vrom=`/`prom=`.

### 10.5 Test-data filenames

The ROM fixtures in `tests/data/roms` (a copy of `gs-test-data`; see
[TEST_DATA.md](../../guide/TEST_DATA.md)) have readable names such as
`iix-iicx-se30-97221136.rom`, but the names are labels for the people who
write `TEST_ROM :=`: nothing parses them and nothing checks them against a
grammar. What a file is comes from the identify surfaces alone. The
`tests/integration/rom-catalog` row checks that every file there is
recognised, and every CPU ROM intact and supported; `scripts/rom-manifest.sh`
and `scripts/test-matrix.py` ask `gs-headless` rather than reading
filenames. Only the browser store gives a ROM a canonical name, its id.
