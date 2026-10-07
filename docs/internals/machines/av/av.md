# The AV family substrate (Quadra 840AV / Centris 660AV)

The **Cyclone/Tempest generation** — Apple's "AV" Quadras — is one platform on
two boards. The shared substrate lives in
[src/machines/av/av.c](../../../../src/machines/av/av.c) /
[av.h](../../../../src/machines/av/av.h); the machines bind it through an
`av_board_t` (per-machine hooks) + `av_board_desc_t` (per-machine data), the
same pattern the GLUE/MDU/OSS/MCU families use. YMCA is the memory controller
that defines the generation, but the chip that defines the *character* of the
platform is the PSC ([psc.md](psc.md)), which absorbs VIA2, the interrupt
controller and every DMA channel.

Machine pages: [q840av.md](q840av.md), [q660av.md](q660av.md).
Device pages: [psc.md](psc.md), [civic.md](civic.md), [cuda.md](cuda.md).
Hardware reference: [av.md](../../../reference/machines/av/av.md) (family),
[q840av.md](../../../reference/machines/av/q840av.md),
[q660av.md](../../../reference/machines/av/q660av.md) (machines) — cited by
section below.

These pages document the **implementation**. Every non-obvious decision below
records the hardware behaviour that forced it, so the reasoning stands on its
own; the models themselves are the normative reference, and each page names the
source file that owns the contract.

## Family traits

(Reference: [av.md](../../../reference/machines/av/av.md) §1–§5 for all of
this, family level.)

- **One ROM, two machines.** Both boards run the same 2 MB image (checksum
  `$5BF10FD1`) mapped at `$40800000` — the first `rom_size = 0x200000` profile
  in the tree. The only identity input is a 4-bit strap nibble the ROM reads
  out of YMCA (§2.4 of the family reference): `$F` = Quadra 840AV (Cyclone,
  40 MHz), `$B` = Centris/Quadra 660AV (Tempest, 25 MHz).
- **Access-triggered ROM overlay.** Out of reset the ROM is readable at
  `$00000000`; the first access to the ROM aperture drops the overlay, RAM
  appears at zero and the aperture becomes direct ROM pages. Unlike GLUE/MDU
  there is no VIA overlay bit, and unlike the MCU family there is no software
  disable **at all** — `YMCA_EPROMmode` is never written and `vOverlay` does
  not exist on this platform (PA4 is reused as `vReqAEnable`). The reset PC
  (`$0000002A`, from ROM offset 4) is a `JMP $40800074`, so the drop happens
  on the very first instruction fetch, before any RAM is touched. (Overlay
  hardware: [av.md](../../../reference/machines/av/av.md) §3.5.)
- **No VIA2, no ASC, no SWIM, no SONIC, no IOPs.** The PSC replaces all of
  them. `config_t.via2` stays NULL and the IPL-2 path is the PSC's 3-register
  pseudo-VIA2 window. VIA1 itself is the generic 6522
  (`src/core/peripherals/via.c`) at island offset 0.
- **CPU-ID register** at `$5FFFFFFC` reads `$A55A2830` and must **not** be
  writable — `GetCPUIDReg` write-probes it, so writes are dropped and the
  probe reads back the constant. Modelled as its own page-sized device window
  at `$5FFFF000`.
- **256 KiB I/O island at `$50F00000`**, mirror mask `$3FFFF`, which also
  serves the non-serialized alias at `$50F40000`. Decode: VIA1 `$0000`,
  PSC-VIA2 window `$2000`, SCC `$4000`, Ethernet address PROM `$8000`,
  53C96 `$18000` (+ Curio's rDMA port at `$18100`), MACE `$1C000`, New Age
  `$2A000`, clock synthesizer `$2E000`, MUNI `$30000`, YMCA `$30400`,
  Sebastian `$30800`, PSC `$31000`, CIVIC `$36000`. CIVIC also answers at
  `$50036000` (the decoder ignores A20–A23 within `$50xxxxxx`), registered as
  a separate region. (Decode: [av.md](../../../reference/machines/av/av.md)
  §3.2/§3.3.)
- **Interrupts**: VIA1→IPL 1, the PSC-VIA2
  window→IPL 2 (**SCSI and floppy arrive here**, not at the PSC level
  registers), MACE→3, SCC/Singer/DMA-complete→4, DSP→5, 60.15 Hz→6, NMI→7.
  The family instantiates the shared `mac030_irq_resolve_ipl` engine with its
  own routing table ([av.md](../../../reference/machines/av/av.md) §5.1).

## YMCA (memory controller, `$50F30400`)

(Hardware reference: family doc §2.3–§2.6 — register model, identification,
speed programming, bank decode.)

Every YMCA register is **one bit wide, accessed as a longword with the value
in bit 31** — which is why the ROM writes `0` or `-1` everywhere. `av.c` stores
one bit per longword slot. The machine-ID straps (`+$38/$3C/$40/$44`) read the
board's nibble; the speed and width registers latch and read back. Their
electrical semantics are undocumented, and the ROM only ever writes three fixed
patterns per clock grade, so accept-and-readback is the correct model, not a
shortcut.

**The bank registers drive the RAM decode** (`av_ymca_remap`). Bank `n`'s
block holds a boundary in MB (bits A20–A26) and a size code: the bank appears
at its boundary in a window of `1 MB << (code − 1)` (code 0 = off), repeating
through it when it is smaller. The ROM sizes memory against exactly that:

- `@YMCASplit` (and power-on) put bank `n` at `n × 16 MB` with 16 MB windows;
- `YMCASizeBanks` finds each bank's size where its signature wraps, twice
  (standard, then wide mode — width is not modelled, both passes agree);
- `YMCAMerge` packs the banks it can use largest first from 0. A bank whose
  size is not a power of two has no code and is switched off.

A bank's ten bits are written boundary first, `Sz2` last, so the decode is
rebuilt on each bank's `Sz2` write; windows it leaves are cleared
(`mac030_clear_page`) so they do not echo.

The installed RAM is the board's banks (`av_ram_banks`): the soldered bank
(660AV: 4 MB as bank 0) and SIMMs of 4/8/16/32 MB, slot `k` on banks
`simm_first_bank + 2k`/`+1` (a 32 MB SIMM is two 16 MB banks). Which banks
the slots are wired to is undocumented; it only decides the order of equal
banks. The flat RAM image holds the banks in merged order, so after
`YMCAMerge` the decode is the image at 0 — what the 040 bus resolver, DMA and
checkpoints address. A total that is no SIMM population of the board is not
in the profile's list, and the build refuses it. This is why RAM presented as
one flat block failed on the 660AV: 28 MB flat is banks of 16 + 12, and the
12 MB bank was switched off.

## MUNI (NuBus bridge, `$50F30000`)

(Hardware reference: family doc §5.4 — NuBus and the MUNI.)

Two latches the boot ROM touches: `IntCntrl` (+`$00`) and `Control` (+`$08`).
The load-bearing behavior is the **absence** case: a 660AV without the NuBus
adapter has no MUNI at all, so reads *and* writes of `MUNI_Control` must raise
a bus error. That is what makes the ROM's `TestForMUNI` clear `MUNIExists`;
a model that answers the read instead leaves the flag wrongly set. The
speed-programming write in `JumpIntoROM` runs under a temporary bus-error
handler and is skipped harmlessly.

## Interrupt-model note (a real bring-up trap)

PSC-VIA2 IFR bit 0 is sometimes described as a "SCSI mirror" of bit 3. Driving it
as a second interrupt source **breaks the boot**: the ROM's level-2 dispatcher
is pattern-indexed, and the extra bit produces IFR combinations
(`$09`/`$29`) it never expects. The SCSI service then runs on a
mis-classifying table entry, the SCSI Manager's deferred-interrupt bookkeeping
is left stale, and the next transaction's select is never issued — the boot
hangs forever inside `SCSIComplete`'s phase wait (whose deadline is
`Ticks + $FFFFFF`, i.e. effectively never). `av_scsi96_irq` therefore drives
**only bit 3**. Chased through the SCSI Manager's own last-interrupt ring at
`$148(A5)`.

## Devices

| Device | Where | Notes |
|---|---|---|
| PSC | [psc.c](../../../../src/machines/av/psc.c) | interrupt controller + 7-channel DMA + sound/DSP latches — [psc.md](psc.md) |
| CIVIC + Sebastian | [civic.c](../../../../src/machines/av/civic.c) | frame buffer, bit-serial registers, RAMDAC — [civic.md](civic.md) |
| Cuda | [cuda.c](../../../../src/machines/av/cuda.c) | ADB/PRAM/RTC behind VIA1's shift register — [cuda.md](cuda.md) |
| New Age | [new_age.c](../../../../src/machines/av/new_age.c) | µPD72070 FDC: the shared chip model (`core/peripherals`) on PSC DMA channel 3 and VIA2 bit 5, driving one internal SuperDrive — [new_age.md](../../core/peripherals/new_age.md) |
| MACE | [mace.c](../../../../src/machines/av/mace.c) | Am79C940 register stub + the Apple address PROM; no datapath, so `.ENET` does not load |
| SCSI | 53C96 (`core/peripherals`) | at `$18000`, `$10` stride; real DMA on PSC channel 0, which the HAL polls |
| SCC | `scc.c` (`core/peripherals`) | single base `$50F04000`, offsets bCtl/aCtl/bData/aData = 0/2/4/6 |

## Testing

- `tests/unit/suites/psc/` — the DMA engine against the three known-good
  register sequences the shipping drivers issue.
- `tests/unit/suites/civic/` — the bit-serial codec, sense protocol, CLUT and
  VBL ack dance.
- `tests/integration/suite-av/` — identity, RAM sizing, video bring-up
  (ROM only), the chime, DSP boot and determinism, the audio-in plug
  contract, checkpoint round-trip, plus the System 7.1 hard-disk desktop
  boot, video-in, beep, TTS, sound-record and speech-recognition rows for
  both machines, and the floppy rows (mount, 800K, write/eject, format and
  a cold boot from the 7.6 install floppy). Every row gates on `have_media()`; the 2 MB ROM and the
  7.1 AV hard-disk image are in gs-test-data (revision 2937e56), so the
  gating is a stale-checkout guard rather than a pending-data placeholder.

## What is deliberately not modelled

GeoPort/DMA serial, Ethernet beyond the register stub, and
NuBus cards in the AV slots.  The DSP3210 **is** modelled — a live core
executing Apple's RTM kernel out of the ROM ([dsp.md](dsp.md)) — as is the
Singer sound datapath in both directions ([singer.md](singer.md):
`machine.sound` with the golden-WAV capture sink, and `machine.audioin`,
the emulator's first microphone surface).  Video *input* is modelled — the
digitizer, its capture path and the browser webcam feed are
[vdc.md](vdc.md); capture is no longer mute: the boot chime and Singer
output play through `audio_out`.
