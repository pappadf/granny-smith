# GS generic NuBus declaration ROM ("GS vROM")

Implementation of `local/gs-docs/proposals/proposal-generic-nubus-vrom.md`:
one declaration-ROM source — a shared 68K driver core plus a thin per-card
personality — built into the emulator, so that every vROM-consuming card
kind has an always-available **generic sibling kind** that needs no dumped
Apple ROM.

## Source and build

```
tools/vrom/
  fetch-vasm.sh         # pinned vasm 2.0f fetch+build (cached in build/vasm)
  Makefile              # assembles 4 personalities, stamps CRC, embeds blobs
  crc.py                # Format Block finaliser (Length/CRC/ByteLanes)
  embed.py              # blobs → committed gsvrom_blobs.h
  gsvrom.s              # top level: directory, board sResource, records
  gsvrom_equ.i          # clean-room equates (no Apple AIncludes)
  gsvrom_macros.i       # OSLstEntry/DatLstEntry/sExec/VPBlock macros
  gsvrom_init.s         # shared PrimaryInit (+ SecondaryInit framework)
  gsvrom_drvr.s         # shared DRVR: csCode dispatch, VBL ISR
  ops_jmfb.s            # JMFB personality (CPB + ops)
  ops_boogie.s          # 24AC personality
  ops_mdcgc.s           # 8•24 GC personality
  ops_se30.s            # SE/30 built-in video personality
```

`make -C tools/vrom` regenerates the **committed**
`src/core/peripherals/nubus/gsvrom_blobs.h`; ordinary emulator builds need
neither vasm nor network.  Images are dense 4-lane (`byteLanes $0F`),
unpadded — the loader tail-places them at the top of the card's declROM
window, and Length/CRC cover exactly the assembled content.

The **shared core** (PrimaryInit control flow, the whole DRVR: Open/Close,
Control 0-9 / Status 2-10, gamma/CLUT bookkeeping, gray fills, the slot-VBL
ISR) is byte-identical across personalities; a personality contributes only
equates, small data tables (monitors, depth codes, fill patterns) and ~8
leaf ops (`ReadSense`, `SetDepth`, `ClutWrite`, `VblEnable/Disable/Ack`,
`FbBase`, `BaseAddr`, `HwInit`).  PrimaryInit and the DRVR each embed their
own copy of the CPB + ops (the Slot Manager copies those blocks to RAM, so
they must be self-contained).

Two hard-won 68K correctness rules baked into the driver:

- the saved `_SwapMMUMode` state lives in **D7**, never in driver storage —
  a VBL ISR that swaps modes inside a handler's swap window would clobber a
  shared memory slot and leave the machine in 32-bit mode;
- every RAM pointer dereferenced inside a 32-bit window is first
  `_StripAddress`ed (24-bit master pointers carry flag bits that address
  nothing in 32-bit mode).

## Emulator integration

Sibling kinds (same HLE model file, `requires_vrom = false`, built-in blob
installed via `declrom_install_builtin` — the offer registry is never
consulted; the built-from record lists the blob path-less as
`builtin:<id>` with its Format-Block CRC):

| Real kind | Generic sibling | notes |
|---|---|---|
| `mdc_8_24` | `8_24` | 4 monitors × 1/2/4/8 bpp; identity gamma (no Kong tint) |
| `display_card_24ac` | `24ac` | 3 multisync monitors; extended-sense probe; 1/4/8/16/32 bpp |
| `824gc` | `8_24gc` | config 0 (640×480); GC accel bring-up works (see gaps) |
| `builtin_se30_video` | `se30` | **SE/30 profile default** — boots with no vROM file |

`video_card=` selects either sibling; on the SE/30 the staged pick may
substitute the BUILTIN slot's kind (BUILTIN-attach kinds only).
`machine.profile` lists both siblings (a BUILTIN slot with siblings is no
longer `fixed`), so the web2 picker shows the pair with the generic one
always enabled.  `vrom.identify` recognises a dumped copy of a generic
image by its stored CRC.

Integration suites: `iicx-gsvrom-video-modes` (JMFB clone of
iicx-video-modes via staged `video_mode=`), `iicx-gsvrom-24ac`,
`iicx-gsvrom-824gc` (accel bring-up ladder), `se30-gsvrom` (Finder boot).

## Known gaps / follow-ups

- **GC visible Finder** (proposal §7.2/§7.3): the 8•24 GC extension
  attaches and the accelerator turns on against the generic ROM (BoardId
  $2C, RevLevel, `Display_Video_Apple_MDCGC` all match), but the screen
  re-point into the super-slot DRAM framebuffer (`ScrnBase $sC011400`)
  needs the real card's quadrant enable/disable state machine that 32-Bit
  QuickDraw's slot-upgrade pass drives ($B0 → $A0 re-open observed on the
  real ROM).  The generic GC currently keeps QuickDraw on the (invisible)
  std-slot VRAM; the accel suites therefore assert bring-up, not pixels.
- **Extended modes (stage 4, 3440×1440@8bpp)**: blocked on the same
  QD32-upgrade mechanism — all supported machines run 24-bit Memory
  Manager under stock System 7, so >1 MB framebuffers are reachable only
  through the QD32 re-open path.  The record/ops plumbing (per-monitor
  VideoBase, f32BitMode families) is in place.
- **24AC direct modes**: 16/32 bpp boot partially validated (16 bpp
  reaches the desktop; colour fidelity needs the §7.2 audit).
- **Stage 5** (`custom_mode=` load-time patching via `declrom_builder`)
  not started; `tools/vrom/crc.py` already implements the CRC finaliser it
  needs.
- The §3.3 mode-table generator (`modes.py`) is deferred until stage 4's
  extended-mode rows exist to generate; monitor tables are currently
  hand-written in the ops files and card models.
