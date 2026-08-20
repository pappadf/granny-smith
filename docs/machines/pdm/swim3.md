# SWIM III and the floppy datapath (`swim3.c`, `swim3_xfer.c`)

SWIM III is Apple's third-generation Sony floppy controller and the one
every Power Macintosh 6100/7100/8100 carries.  All three models have the
same subsystem — one internal manual-inject SuperDrive, no external port —
so this is one piece of hardware for the whole family.

The model is in three parts:

| Part | File | What it owns |
|---|---|---|
| register file + drive protocol | `src/machines/pdm/swim3.c` | the 16 registers, their readback / read-to-clear semantics, the interrupt contract, and the Sony drive-register sense/strobe protocol |
| transfer engine | `src/machines/pdm/swim3_xfer.c` | header hunt, sector read and write, whole-track format, raw capture, the GCR nibble codec, and the rotational model |
| DMA channel | `src/machines/pdm/amic.c` | the AMIC floppy channel: window addressing, the 16-bit count, the DMA-complete interrupt |
| drive and media | `src/core/peripherals/floppy.c` | head position, motor, the disk image, `machine.floppy.*` |

Sources: Apple SWIM3 ERS v1.2 (3/24/93); the ISM ASIC specification rev
4.1 and SWIM2 ERS rev 3.1 for lineage; Apple, *Power Macintosh Computers*
Developer Note (1994), pp. 15–23 and Table 3-7; the Power Macintosh 8100
schematic set (051-0333 rev A, sheets 12 and 20); and Apple, *Guide to the
Macintosh Family Hardware*, 2nd ed., for the GCR sector format and speed
zones.

## Why the chip must exist even with no floppy

Worth stating before anything else, because it cost a debugging session
during the family's bring-up: **SWIM3 is load-bearing for any System boot,
including a SCSI boot with no floppy media at all.**  With `$50F16000`
unwired, the ROM `.Sony` driver's open fails its presence probe, tears
down, and leaves `SonyVars` (`$134`) = −1 — and System 7.5 later
dereferences `SonyVars−$C` unconditionally, producing a wild jump and a
bomb a long way from the cause.

## Registers

Sixteen byte-wide registers at `$50F16000`, **stride `$200`** (index =
offset >> 9; the chip's A0–A3 hang off `BufAddr<9..12>`).  Three indices
read and write different things.

| # | Offset | Write | Read |
|---|---|---|---|
| 0 | `$0000` | Data (FIFO) | Data — PDM never uses PIO; the driver has no vector for it |
| 1 | `$0200` | Timer | Timer |
| 2 | `$0400` | — | Error, **read-to-clear** |
| 3 | `$0600` | ParamData (write precompensation; `$95` = nominal) | — |
| 4 | `$0800` | Phase — CA0/CA1/CA2/LSTRB | Phase, **reads back what was written** |
| 5 | `$0A00` | Setup | Setup |
| 6 | `$0C00` | Zeroes — clears the 1-bits of Mode | Mode |
| 7 | `$0E00` | Ones — sets the 1-bits of Mode | Handshake |
| 8 | `$1000` | — | Interrupt, **read-to-clear** |
| 9 | `$1200` | Step | Step |
| 10 | `$1400` | CurTrack | CurTrack — cylinder in 6:0, head in bit 7 |
| 11 | `$1600` | CurSect | CurSect — sector in 6:0, `Last_ID_valid` in bit 7 |
| 12 | `$1800` | Gap (gap-3 pad requests) | FormatByte — the header's 4th byte |
| 13 | `$1A00` | FirstSector | FirstSector |
| 14 | `$1C00` | SectorsToXfer | SectorsToXfer |
| 15 | `$1E00` | IntMask | IntMask |

Four readback properties are gates the driver will not get past:

1. **Phase reads back.**  `InitSWIMChip` loops the patterns `$05`, `$06`,
   `$07` through it as the chip-presence test; a mismatch is `initIWMErr`
   and the `SonyVars` bomb above.
2. **Setup reads back**, and `LoadSWIMparams` compares after writing.
3. **IntMask is read-modify-write** — the driver uses `bset`/`bclr` on it.
4. **Interrupt and Error clear on read**, and are often read with `tst.b`
   purely for that side effect.  Handshake bit 1 mirrors "an enabled
   interrupt is pending" and bit 5 mirrors "Error is non-zero".

There is **no IWM mode and no ISM compatibility**: the ISM's mode bit 6
(ISM/IWM select) is FormatMode here, there is no four-write entry
sequence, and no `Q6L/Q7H` addressing.  Presenting IWM behaviour at
`$50F16000` would be wrong.

### Mode bits (`Zeroes` / `Ones` / `Mode`)

`$01` EnableInts · `$02` Drive1Enabled · `$04` Drive2Enabled · `$08`
StartAction (GO) · `$10` WriteMode · `$20` HeadSelect (the drive SEL line)
· `$40` FormatMode · `$80` GoStep.

Two ordering rules the hardware imposes and the driver relies on:
WriteMode is only ever set while GO is clear, and stopping a format clears
GO **first**, then FormatMode.

### Setup bits

`$02` CopyProtMode (raw) · `$04` GCRMode · `$08` ClockDiv2 · `$10`
DisGCRConv · `$20` IBMDrive · `$40` GCRWrites · `$80` SoftReset.  The
driver's per-format values are `$4C` for every GCR format and `$28` for
MFM 720K/1440K — which is how this model knows which framing the chip is
in (see "Format detection" below).

### Interrupts

One flag each for the timer (`$01`), step complete (`$02`), an address
header read (`$04`), a transfer done (`$08`) and a Sense-line transition
(`$10`).  A source sets its flag regardless of the mask; the IRQ line
asserts when `EnableInts && (Interrupt & IntMask)`, and reading Interrupt
clears every flag and drops the line.

The line itself is **pseudo-VIA2 device bit 5**, dispatched at 68k level
2 — *not* the AMIC DMA interrupt.  Which of the two paths a transfer ends
on matters (see below).

## The drive

Sixteen 1-bit status registers and eight strobe registers, addressed by
`{SEL, CA2, CA1, CA0}` where SEL is Mode bit 5 and CA0–2 are Phase bits
0–2.  A sense read puts the addressed bit on the drive's RdData line,
which the driver samples at **Handshake bit 2** — bit 2, not the ISM's
bit 3.  A strobe is a rising edge on LSTRB (Phase bit 3).

The emulated drive is a SuperDrive: `rNoDrive` = 0, `rDoubleSided` = 1,
`rMFMDrive` = 1, which is the `x011` pattern `FindDriveKind` decodes as
`DSMFMGCRDriveKind`.  Drive 2 senses absent at every address, because the
family has no second bay.

Media-dependent answers come from the shared floppy module: disk-in-place
(8), write-protect (9), track-0 (10), index/tach (11), ready (14), and
HD-versus-DD media (15).  Addresses 4 and 12 do double duty — addressing
them *routes* head 0 or head 1 for the next transfer, and during a GCR
format the driver uses 1 and 15 for the same purpose because those read
back as 1 while it writes from the index.

## AMIC DMA

Floppy data never moves by PIO on this family; there is no fallback if the
DMA channel is missing.  The channel's registers are at `$50F32060`
(address), `$50F32064` (16-bit count) and `$50F32068` (control).

Two things about its addressing are easy to get wrong and both are
guest-visible:

- **`DMARST` restores the full physical address of the floppy region**,
  which is the DMA window's base plus `$15000` — not the bare offset.  The
  driver resets the channel at open, reads the address back, reads the
  window base from `$50F31000`, and computes its whole track-cache pointer
  as `$61000000 + (readback − base)`.  The ROM programs a non-zero window
  base (`$05780000` at HWInit), so a model that answers `$15000` hands the
  driver a pointer into nowhere.
- **Only the low 16 bits advance.**  The channel is hard-wired into the
  window's second 64 KB, and during operation the driver rewrites only the
  low half of the address register.  A transfer wraps inside that 64 KB
  rather than walking into the next channel's region.

Transfers terminate on one of two different interrupts, and the driver
chooses per operation:

| Operation | Terminated by | Because |
|---|---|---|
| sector read / write, format | **SWIM3's own IRQ** (VIA2 bit 5) | the count is set to a huge `$8000` and `DMAIE` is left off; the chip says when the sector is done |
| raw / copy-protect read | **DMA complete** (`DMAIF` → AMIC DMA ISR0 bit 6, level 4) | there is no sector structure to end on, so the count is what stops it |

Getting these backwards produces a machine that hangs only on
copy-protected reads, or one that double-completes every sector.

## The transfer engine

### Sector level, not flux

The engine models the disk at the **sector** level and reads and writes the
image directly.  This is not a shortcut around the hardware — it is where
the hardware already is.  SWIM3 does header parsing, sector matching, CRC
and the GCR 6↔8 conversion itself, and the only thing guest software ever
observes is the DMA byte stream, whose contents the ERS specifies byte for
byte:

| Encoding | What a sector read deposits |
|---|---|
| MFM | exactly the 512 decoded data bytes — no header echo, no CRC |
| GCR | 1 sector byte, then 703 six-bit values: 12 tag bytes + 512 data bytes as 699 nibbles, then 4 checksum nibbles |

Then `Gap`-many pad requests, which the driver always sets to zero.

The cost of the choice, stated so a future reader can weigh it: media that
depend on flux timing or non-standard sector layouts do not work, and a
damaged image reads as a clean error rather than as a read that wanders.
That is the same trade the IIfx's `iop_swim.c` and the SE/30's
`floppy_swim.c` already make.

### Rotation is modelled

Two guest behaviours observe how fast the disk turns, so the head sees one
sector's worth of track per (revolution ÷ sectors-per-track): 300 rpm in
MFM, and the zone's rpm (394…590) in GCR.

1. The driver's timeouts — 300 ms per track operation, ~8 ms of sector
   data time, 1 s minimum per track request.
2. The GCR format routine, which **self-tunes its intersector sync count**
   by measuring rotational wrap with `_GetMicroSeconds`.  An emulator
   whose format timing is wildly fast makes that loop shrink the gap down
   to `GCRMinSyncCount` and fail with `fmt1Err`/`fmt2Err`.

While GO is set in read mode, every header that passes updates
CurTrack/CurSect/FormatByte and raises the ID interrupt; a header that
matches `FirstSector` with `SectorsToXfer` non-zero additionally streams
its data field and decrements the counter.  `SectorsToXfer` is a real
counter, visible to software after an error, not a boolean.

### Format detection falls out of the geometry

The driver's search order is **MFM1440K, MFM720K, GCR800K, GCR400K,
GCRonHD**, and every wrong-mode attempt has to *fail* — not return a
plausible-looking wrong header.  Here it fails the way it does on
hardware: the engine presents address fields only when the chip's framing
(Setup bit 2, GCRMode) matches the encoding the media actually carries, so
a wrong-mode attempt sees nothing, no ID interrupt arrives, and the
driver's 300 ms timeout fires.  This also covers the `<SM16>` pitfall —
reading an MFM header in GCR mode must not error, but must not look valid
either.

Geometry comes from the image's size alone:

| Size | Encoding | Sides | Sectors/track | Format byte |
|---|---|---|---|---|
| 1440 KB | MFM | 2 | 18 | `$02` (512-byte sectors) |
| 720 KB | MFM | 2 | 9 | `$02` |
| 800 KB | GCR | 2 | 12…8 by zone | `$22` |
| 400 KB | GCR | 1 | 12…8 by zone | `$02` |

Sector numbers on the media are 1-based in MFM and 0-based in GCR; the
hardware compares `FirstSector` against the raw header value, and the
driver does the ±1 itself.

### Writes and formats are the same parser

Both put a driver-built byte stream on the DMA channel in which `$99`
introduces a command:

| Escape | Meaning |
|---|---|
| `99 0F` | pass the next 512 bytes literally — the data field |
| `99 04` | write both CRC bytes (computed and discarded here: we store decoded sectors) |
| `99 08` | end data, terminate the transfer |
| `99 A1` / `99 C2` / `99 FB` / `99 FE` | the missing-clock mark bytes |
| `99 99` | a literal `$99` data byte |

In GCR mode the same escapes work, values below `$40` go through the
hardware encode table, and a byte with the **high bit set is a literal
pattern** — which is why the driver's `3F BF 1E 34 3C 3F` is the
spec-sanctioned spelling of the canonical sync group `FF 3F CF F3 FC FF`.

A **write** parses one data field at the sector whose header matched.  A
**format** parses a whole track image and takes each data field's sector
number from the address field that preceded it — so what a format actually
does here is lay down the layout the stream declares.

### Raw / copy-protect capture

With CopyProtMode set the chip streams **two bytes per disk byte**: a flag
(`$00`, or `$80` for a mark byte) then the data byte, from the next mark
byte until GO drops or the DMA count exhausts.  The engine synthesises
that stream from the sector model, emitting only the two documented flag
values (real silicon also emits others — a known chip bug — which software
skips anyway).

## Machine seams

`pdm_init` creates the drive with `floppy_init(FLOPPY_TYPE_SWIM3, NULL,
…)` — the NULL map is deliberate: PDM decodes SWIM3 through the AMIC
island, not through a memory-mapped floppy region, so the shared module
carries only the drive and the media.  `pdm_fd_insert` / `pdm_fd_present`
forward to it for drive 0 and refuse drive 1, which does not exist on this
family.

Checkpointing: the floppy block is appended after the SCSI blocks and
before the HMC/AMIC tail, and PDM now also saves the image list
(`mac_checkpoint_save_images`) — without it a restore had nothing to
resolve media filenames against, and the SCSI restore dereferenced a null
image.

## Known gaps

- The Timer register stores and reads back but does not count down.  The
  PDM `.Sony` driver never uses it (it uses the Time Manager); Linux's
  driver does.
- Write precompensation (`ParamData`) is accepted and ignored — it shifts
  transitions by fractions of a bit cell, which a sector model cannot
  observe.
- The AMIC floppy channel's "flush" bit (control bit 4) is a latch; its
  floppy-side behaviour is undocumented and the driver does not use it.
- Copy-protected media that check flux timing do not work, by design
  (see "Sector level, not flux").
