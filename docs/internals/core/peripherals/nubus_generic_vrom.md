# Generic NuBus video declaration ROM

A NuBus video card is invisible to Mac OS without its declaration ROM (the
on-card firmware the Slot Manager reads at startup; see
[declaration-rom.md](../../../reference/hardware/nubus/declaration-rom.md) for the byte layout and the data structures
it contains). On real hardware that ROM is soldered onto the card, so every
card model our NuBus code emulates historically needed a *dumped* copy of
the corresponding Apple ROM to be supplied before the card would work — with
no dump the Slot Manager finds a zero-filled window, skips the slot, and the
machine boots with no video.

This document describes the **generic declaration ROM** — each card's
*substitute ROM*: firmware we write and build into the emulator so those
cards work with no user-supplied ROM file at all. It is not a dump of any Apple ROM; it is a fresh implementation
of the same interface — the Slot Manager data structures plus a Mac OS video
driver — written against the published declaration-ROM contract.

## One ROM, many card models

The register interfaces of the emulated cards differ sharply: the 8•24
(JMFB) exposes 16-bit registers in a `+$200000` block, the Display Card 24AC
uses byte-wide registers on lane 3 at high slot offsets with an active-low
RAMDAC, the 8•24 GC drives an accelerator in super-slot space with a RISC
heartbeat, and the SE/30 built-in video is a bare framebuffer with no
registers at all. A single ROM image can only drive one of these interfaces,
so we build **one image per card model**.

The insight that keeps this maintainable is that those images are almost
entirely the same program. The Slot Manager data structures, the whole
`PrimaryInit` bring-up flow, and the entire runtime video driver are dictated
by the OS, not by the card — only the handful of leaf routines that actually
touch a register differ. So the source is organised as:

- a **shared core** — one copy, byte-identical in every image: the sResource
  directory framework, `PrimaryInit` control flow (read the monitor sense →
  prune the declared modes to the sensed monitor → program the boot depth →
  gray the screen → default the slot PRAM), and the complete video `DRVR`
  (Open/Close, the Control 0–9 / Status 2–10 `csCode` dispatch, gamma and
  CLUT bookkeeping, gray fills, and the slot-VBL interrupt handler); and

- a per-model **personality** — the small amount that is card-specific:
  register addresses, a few data tables (the monitor list, depth codes, gray
  fill patterns), and about eight leaf routines the shared core calls through
  by name: `ReadSense`, `SetDepth`, `ClutWrite`, `VblEnable`/`VblDisable`/
  `VblAck`, `FbBase`, `BaseAddr`, and `HwInit`.

The shared core never names a register address; it drives the hardware only
through the personality's leaf routines. Adding support for another card
model is therefore a new personality (a few register equates, its tables,
its eight routines), not a new ROM.

There are four personalities today — **JMFB** (8•24), **Boogie** (Display
Card 24AC), **MDCGC** (8•24 GC), and **SE30** (built-in video).

A structural constraint worth knowing when reading the source: the Slot
Manager copies each executable block (`PrimaryInit`, the `DRVR`) out of the
ROM and into RAM before running it, so each block must be self-contained.
`PrimaryInit` and the `DRVR` therefore each embed their **own** copy of the
personality's leaf routines rather than sharing one copy in ROM data space.

## Where the bytes come from: generated records, spliced code

The declaration ROM is **not** a committed image. It is built at machine
construction, in two halves:

- The **declarative records** — the sResource directory, the board
  sResource (BoardId, vendor strings, PRAM defaults), and one functional
  video sResource per monitor with its `mVidParams` (PixMap) blocks — are
  *generated in C* by the declaration-ROM builder (`declrom.c`), fed from
  the card kind's `monitors[]` table. That table is the **single source of
  truth for mode geometry**: `gsvrom_generate` reads each monitor's width,
  height, and depth list straight out of it, so a mode declared to the OS
  and a mode the HLE displays can never disagree. The builder serialises
  the image bottom-up (every record before the list that points at it, so
  every offset is a backward self-relative reference) and computes the
  Format-Block CRC in C — the job the old `crc.py` did at build time.

- The **68K code blocks** — `PrimaryInit`, the `DRVR`, and the GC's
  `SecondaryInit` — are per-personality fragments under
  `src/core/peripherals/nubus/vrom68k/`, assembled by the core build (see
  below) and spliced into the generated image verbatim. Because the Slot
  Manager copies each block to RAM before running it, the fragments are
  self-contained; the builder only wires the directory entries that point
  at them. What the fragments still carry beyond code is *logic-only*
  data: the sense-probe strategy, the per-depth gray-fill patterns, and a
  bare list of the personality's top-level video spIDs (the sister ids the
  emulator seeds into PRAM) that `PrimaryInit`'s prune walks. Mode
  geometry is **not** in the fragments — `PrimaryInit` and the `DRVR` read
  the boot mode's `vpRowBytes`/bounds back out of the generated records
  through the Slot Manager (`sRsrcInfo` → `sFindStruct` → `sReadStruct`),
  exactly as the genuine Apple drivers do.

### Building the 68K fragments

The fragments are ordinary targets of the main build. `Makefile` and
`Makefile.headless` (and so the wasm and headless builds) both include
`src/core/peripherals/nubus/vrom68k/vrom68k.mk`, which assembles each
`frag_<block>.s` per personality with **GNU binutils targeting m68k**
(`m68k-linux-gnu-as` + `objcopy -O binary`, from the
`binutils-m68k-linux-gnu` package; any m68k-targeted binutils works —
override `M68K_AS`/`M68K_OBJCOPY`) and embeds the raw blocks into a
generated header `build/vrom68k/gsvrom_fragments.h` (never committed).
Editing a `.s` file rebuilds fragment → header → object with real
dependency tracking. There is deliberately **no** fallback when the
assembler is absent: the build fails with an actionable message rather
than producing environment-dependent content. The source is gas syntax
in `.altmacro` mode; two port hazards are fenced in the macros —
list-entry offsets use `+` not `|` (gas silently mis-folds a masked
forward difference OR'd with a constant), and strings use
`.ascii`/`.asciz` (gas's `dc.b` silently drops string operands).

## Two 68K correctness rules

Two non-obvious hazards, both fixed in the shared driver and worth calling
out because they are easy to reintroduce:

- The saved `_SwapMMUMode` state is kept in a **register (D7)**, never in the
  driver's private storage. The card's registers live above the 1 MB minor
  slot space, so the driver enters 32-bit addressing to touch them; a VBL
  interrupt that fires inside such a window swaps modes too, and a shared
  memory slot would be clobbered — leaving the machine stuck in 32-bit mode.
- Every RAM pointer dereferenced inside a 32-bit window is first passed
  through `_StripAddress`. On a 24-bit machine a Memory Manager master
  pointer carries flag bits in its high byte; those bits address nothing once
  the CPU is in 32-bit mode, so an unstripped pointer reads garbage.

## How the emulator uses it

Each card that consumes a declaration ROM is **one card kind with two ROMs**:
Apple's, loaded from a dump, and this generated one, its **substitute ROM**
(`nubus_card_kind_t.substitute`). Both drive the *same* emulated hardware
model; only the declaration ROM differs:

| Card kind | Personality | Substitute ROM's monitors |
|---|---|---|
| `mdc_8_24` | JMFB | 4 monitors × 1/2/4/8 bpp; identity gamma |
| `display_card_24ac` | Boogie | 3 multisync monitors; extended-sense probe; 1/4/8/16/32 bpp |
| `824gc` | MDCGC | config 0 (640×480) only (`substitute_monitors`); GC accelerator bring-up (see gaps) |
| `builtin_se30_video` | SE30 | the built-in 9″ screen |

Which ROM a card runs is decided once, as the bus seats it
(`nubus_entry_substitute`): the substitute when the slot asks for it
(`rom=substitute` in a `slots=` entry), when it has a custom geometry, or
when Apple's ROM is not offered; Apple's otherwise. The seat records the
choice (`slot_opts_t.substitute`), the card's name says it ("Macintosh
Display Card 24AC (substitute ROM)"), and `machine.nubus.slot[N].card.substitute`
reads it. At `card_init` the card calls `gsvrom_generate` to build the image
and installs it directly (`declrom_install_builtin`) rather than consulting
the offer registry. The finished image is laid out exactly like a
file-backed chip — tail-placed at the top of the card's declaration-ROM
window per its `byteLanes` byte — and the card names it path-lessly, as
`builtin:<card id>` (`machine.nubus.slot[N].card.declrom.path`, with its CRC
in `.crc`). Generation is deterministic within one emulator build, so a
checkpoint carries no image for it: the card's block holds Apple's chip, or
nothing for the substitute, which a restore regenerates from the seat the
bus's part carries (card, mode, custom geometry); the CRC is informational,
since it varies with the mode set and with the binutils version that
assembled the fragments.

Selection and identification:

- `catalog.profile` reports a card whose ROM is not offered as status
  `substitute` (it boots with this ROM); the dialog says so and offers the
  upload.
- `custom=WxHxD` in a slot entry (`custom_mode=` for the first socket; the
  8•24 today) boots the card's default monitor at a user-chosen resolution
  on the substitute ROM: the generated records carry a video sResource at
  that geometry and the HLE displays it. The spec is validated at boot
  (well-formed `WxHxD`, supported depth, width a multiple of 32,
  `rowBytes < $4000`) and against the card's framebuffer window
  (`custom_mode_fits`). Modes larger than the 1 MB minor window need the QD32
  re-open path (see gaps) and are rejected.
- `vrom.identify` recognises a dumped copy of one of these generated images
  **structurally** — a right-sized image whose board sResource carries the
  `granny-smith` VendorId, keyed to its card by its BoardId. A generated
  image has no fixed CRC to match.

### Tests

- `iicx-gsvrom` — a row suite covering the personalities on the generated
  vROM: `gsvrom-sweep` (the 10-cell JMFB sweep — 13" RGB at 1/2/4/8 bpp, 12"
  RGB, 15" portrait, 21" two-page — each a `slots="9=mdc_8_24,rom=substitute,mode=…"`
  entry, a re-run of `iicx-video-modes` against the substitute ROM), `gsvrom-24ac` (the Boogie
  personality at 640×480×8 and 832×624×8), `gsvrom-824gc` (the MDCGC
  personality's accelerator bring-up ladder: attach → boot → arm → gc-on),
  and `gsvrom-custom-mode` (the JMFB personality booting at a `custom=`
  resolution (800×600×8) that fits the minor window).
- `suite-se30` row `se30-701-gsvrom` — the SE30 personality booting to the
  Finder on the substitute ROM.
- `boot-vrom-unoffered`, `checkpoint-card-roms` — a card with no ROM offered
  boots its substitute; a checkpoint restores the ROM it carries.

A host-side unit suite (`tests/unit/suites/declrom`) generates every
personality's image without booting a guest and checks its structure
(directory walk, ascending ids, no zero offsets, verbatim fragment
splicing), identity (BoardId, vendor string, sResource names), and
regeneration determinism.

## Known gaps

- **8•24 GC visible Finder.** The 8•24 GC accelerator extension attaches and
  the accelerator turns on against the substitute ROM (its BoardId `$2C`,
  RevLevel string, and `Display_Video_Apple_MDCGC` driver name all match what
  the extension keys on). But the card's visible framebuffer lives in
  super-slot DRAM, which 24-bit QuickDraw cannot address; moving the screen
  there (`ScrnBase = $sC011400`) is done by 32-Bit QuickDraw's slot-device
  upgrade pass, which re-opens the driver on a 32-bit-addressed sResource
  family (a `$B0 → $A0` re-open observed on the real ROM). Reproducing what
  that pass keys on in the ROM is unfinished, so the substitute GC keeps
  QuickDraw on the (correct but off-screen) standard-slot VRAM; the
  accelerator suites assert bring-up, not pixels.
- **Extended resolutions** (e.g. 3440×1440 at 8 bpp) are blocked on the same
  mechanism: under stock System 7 every supported machine runs in 24-bit
  Memory Manager mode, so a framebuffer larger than the 1 MB minor slot
  window is only reachable through the same 32-Bit-QuickDraw re-open path. The
  declarative plumbing (per-monitor base offsets, 32-bit-addressed sResource
  families) is in place.
- **24AC direct colour.** 16/32-bpp boot is partially validated — 16 bpp
  reaches the desktop; colour fidelity at the direct depths is unverified.
- **Custom resolutions on other cards.** A custom geometry is wired for the
  8•24's substitute ROM (JMFB); the other cards take none, and a boot
  document that gives one is rejected (their kinds have no
  `custom_mode_fits`).
- **The 8•24 GC's 16″ configuration** rides the GC-OS VidComm channel and is
  not in its substitute ROM yet, so a 16″ mode on the substitute is refused.

## See also

- [declaration-rom.md](../../../reference/hardware/nubus/declaration-rom.md) — the declaration-ROM byte layout and Slot
  Manager contract this firmware implements.
- `src/core/peripherals/nubus/vrom68k/` — the 68K fragment sources and
  `vrom68k.mk` (assembled by the core build; no separate toolchain).
