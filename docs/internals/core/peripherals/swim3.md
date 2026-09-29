# SWIM III — the shared floppy controller model (`swim3.c`, `swim3_xfer.c`)

SWIM III is Apple's third-generation Sony floppy controller: every Power
Macintosh 6100/7100/8100, the 7500/8500/9500 family, and the Network
Servers carry one, always driving a single internal manual-inject
SuperDrive.  The chip is the same part everywhere; what differs per board
is how it is decoded, which DMA engine feeds it and where its interrupt
lands.  So the chip lives here, once, and each board supplies a small
backend:

| Part | File | What it owns |
|---|---|---|
| register file + drive protocol | `src/core/peripherals/swim3.c` | the 16 registers, their readback / read-to-clear semantics, the interrupt contract, the Sony drive-register sense/strobe protocol, head routing |
| transfer engine | `src/core/peripherals/swim3_xfer.c` | header hunt, sector read and write, whole-track format, raw capture, the GCR nibble codec, the rotational model |
| drive and media | `src/core/peripherals/floppy.c` | head position, motor, the disk image, `machine.floppy.*` |
| board backend | `src/machines/pdm/swim3.c`, `src/machines/tnt/swim3.c` | register stride, the DMA byte movers, the interrupt sink |

Sources: Apple SWIM3 ERS v1.2 (3/24/93); the ISM ASIC specification rev
4.1 and SWIM2 ERS rev 3.1 for lineage; Apple, *Power Macintosh Computers*
Developer Note (1994), pp. 15–23 and Table 3-7; Apple, *Guide to the
Macintosh Family Hardware*, 2nd ed., for the GCR sector format and speed
zones; Linux `drivers/block/swim3.c` and Open Firmware's own `swim3`
package for how the Grand Central boards drive it.

## The backend contract (`swim3_backend_t`)

```c
bool (*dma_running)(void *ctx);            // a channel is open for the transfer
bool (*dma_get)(void *ctx, uint8_t *out);  // one byte to write, or false
bool (*dma_put)(void *ctx, uint8_t value); // one read byte toward memory, or false
void (*set_irq)(void *ctx, bool level);    // the chip's IRQ pin
```

The engine sees only "one byte, yes or no".  A `false` is what the chip's
own FIFO would report — an underrun on write, an overrun on read — and
ends the transfer with the matching Error bit.  The PDM answers from the
AMIC floppy channel (`pdm_amic_fd_dma_*`); the TNT family answers from a
byte ring in front of DBDMA channel 1 (`tnt/swim3.c`), which turns the
engine's push into the DBDMA engine's pull.

`swim3_t` is plain data first — a board checkpoints it positionally
inside its own state — followed by the drive, scheduler and backend
pointers, which the board re-binds with `swim3_bind()` after a restore.

## Registers

Sixteen byte-wide registers, addressed here by **index**; the board
decodes its own stride (`$200` on the PDM, index = offset >> 9; `$10`
behind Grand Central, index = offset >> 4).  Three indices read and write
different things.

| # | Write | Read |
|---|---|---|
| 0 | Data (FIFO) | Data — the Mac OS driver never uses PIO; the model has none |
| 1 | Timer | Timer — the running count, live |
| 2 | — | Error, **read-to-clear** |
| 3 | ParamData (write precompensation; `$95` = nominal) | — |
| 4 | Phase — CA0/CA1/CA2/LSTRB | Phase, **reads back what was written** |
| 5 | Setup | Setup |
| 6 | Zeroes — clears the 1-bits of Mode | Mode |
| 7 | Ones — sets the 1-bits of Mode | Handshake |
| 8 | — | Interrupt, **read-to-clear** |
| 9 | Step | Step |
| 10 | CurTrack | CurTrack — cylinder in 6:0, head in bit 7 |
| 11 | CurSect | CurSect — sector in 6:0, `Last_ID_valid` in bit 7 |
| 12 | Gap (gap-3 pad requests) | FormatByte — the header's 4th byte |
| 13 | FirstSector | FirstSector |
| 14 | SectorsToXfer | SectorsToXfer |
| 15 | IntMask | IntMask |

Readback properties a driver will not get past without:

1. **Phase reads back.**  The Mac OS `InitSWIMChip` loops `$05`, `$06`,
   `$07` through it as the chip-presence test.
2. **Setup reads back**, and `LoadSWIMparams` compares after writing.
3. **IntMask is read-modify-write.**
4. **Interrupt and Error clear on read**, often read with `tst.b` purely
   for the side effect.  Handshake bit 1 mirrors "an enabled interrupt is
   pending" and bit 5 mirrors "Error is non-zero".
5. **The Timer counts.**  A loaded value decrements at 1 MHz, the running
   count reads back live, and TIMER_DONE fires at zero.  The Mac OS
   driver never touches it; Copland's floppy plugin and **Open Firmware's
   swim3 package** pace every drive-register access with it (load,
   poll to zero, proceed).

There is no IWM mode and no ISM compatibility: no four-write entry
sequence, no `Q6L/Q7H` addressing.

### Mode bits (`Zeroes` / `Ones` / `Mode`)

`$01` EnableInts · `$02` Drive1Enabled · `$04` Drive2Enabled · `$08`
StartAction (GO) · `$10` WriteMode · `$20` HeadSelect (the drive SEL line)
· `$40` FormatMode · `$80` GoStep.

### Setup bits

`$02` CopyProtMode (raw) · `$04` GCRMode · `$08` ClockDiv2 · `$10`
DisGCRConv · `$20` IBMDrive · `$40` GCRWrites · `$80` SoftReset.  The
Mac OS driver's per-format values are `$4C` for every GCR format and `$28`
for MFM 720K/1440K (Open Firmware uses `$28` too), which is how the
engine knows which framing the chip is in.

### Interrupts

One flag each for the timer (`$01`), step complete (`$02`), an address
header read (`$04`), a transfer done (`$08`) and a Sense-line transition
(`$10`).  A source sets its flag regardless of the mask; the IRQ line
asserts when `EnableInts && (Interrupt & IntMask)`, and reading Interrupt
clears every flag and drops the line.  Where the line goes is the
board's: pseudo-VIA2 device bit 5 on the PDM, Grand Central interrupt 19
on the TNT boards.

## The drive

Sixteen 1-bit status registers and eight strobe registers, addressed by
`{SEL, CA2, CA1, CA0}` where SEL is Mode bit 5 and CA0–2 are Phase bits
0–2.  A strobe is a rising edge on LSTRB (Phase bit 3).

**A sense read answers on two handshake bits.**  The ERS lists bit 2
"RDData — direct read of drive data" and bit 3 "Sense — direct read of
rddata input"; the Mac OS `.Sony` driver samples bit 2, Open Firmware's
swim3 package and Linux's driver sample bit 3 (`stat & DATA`, `DATA =
0x08`).  The model drives both.  (A model that drove only bit 2 answered
every Open Firmware sense with 0 — "drive present, disk in" by luck,
"single-sided" by the same luck — and the firmware's `open` ended in
`BAD DISK`.)

The emulated drive is a SuperDrive: `rNoDrive` = 0, `rDoubleSided` = 1,
`rMFMDrive` = 1, the `x011` pattern the Mac OS `FindDriveKind` decodes as
`DSMFMGCRDriveKind`.  Drive 2 senses absent at every address.

Media-dependent answers come from the shared floppy module: disk-in-place
(8), write-protect (9), track-0 (10), index/tach (11), ready (14), and
HD-versus-DD media (15).

**Head routing.**  Addresses 4 and 12 (RdData0 / RdData1) are also the
head select: whatever the SEL/CA lines address at transfer time is the
head whose data the chip decodes.  The model routes whenever Phase or
HeadSelect changes, not only when a sense is read — the Mac OS driver
reads the sense there, Open Firmware just sets the lines and starts the
transfer, and a model that routed only on the read gave side 0 for every
side-1 block.  During a GCR format the driver uses addresses 1 and 15 for
the same purpose (they read back as 1 while it writes from the index).

## The transfer engine

### Sector level, not flux

The engine models the disk at the **sector** level and reads and writes
the image directly.  SWIM3 does header parsing, sector matching, CRC and
the GCR 6↔8 conversion itself, and the only thing guest software ever
observes is the DMA byte stream, whose contents the ERS specifies byte
for byte:

| Encoding | What a sector read deposits |
|---|---|
| MFM | exactly the 512 decoded data bytes — no header echo, no CRC |
| GCR | 1 sector byte, then 703 six-bit values: 12 tag bytes + 512 data bytes as 699 nibbles, then 4 checksum nibbles |

Then `Gap`-many pad requests (drivers set it to zero).  The cost: media
that depend on flux timing or non-standard sector layouts do not work,
and a damaged image reads as a clean error.  The same trade the IIfx's
`iop_swim.c` and the SE/30's `floppy_swim.c` make.

### Rotation is modelled

The head sees one sector's worth of track per (revolution ÷
sectors-per-track): 300 rpm in MFM, the zone's rpm (394…590) in GCR.  Two
guest behaviours observe it: the drivers' per-sector and per-track
timeouts, and the Mac OS GCR format routine, which self-tunes its
intersector sync count by measuring rotational wrap.

While GO is set in read mode, every header that passes updates
CurTrack/CurSect/FormatByte and raises the ID interrupt.  A header that
matches `FirstSector` (bit 6 = match any) with `SectorsToXfer` non-zero
streams its data field and decrements the counter — and **the remaining
sectors are "accessed continuously"** (ERS register `$E`): once the first
has matched, every following header on the track is the next sector, no
match required.  Open Firmware and Linux read a track's tail in one
transfer this way (`FirstSector = n`, `SectorsToXfer = spt − n + 1`); the
Mac OS driver's one-sector transfers never see the difference.  The
counter is real and visible after an error, not a boolean.

The engine's service slots land exactly on header boundaries; the slot
delay is rounded up and the header index is computed with a small
nudge, because a slot a fraction of a nanosecond early would name the
header just delivered a second time — and a continuous transfer would
hand that duplicate to the driver as the next sector (seen as corrupted
program loads from Open Firmware before the fix).

### Format detection falls out of the geometry

The Mac OS driver's search order is MFM1440K, MFM720K, GCR800K, GCR400K,
GCRonHD, and every wrong-mode attempt has to fail: the engine presents
address fields only when the chip's framing (Setup bit 2) matches the
media, so a wrong-mode attempt sees nothing and the driver's timeout
fires.  Geometry comes from the image's size alone:

| Size | Encoding | Sides | Sectors/track | Format byte |
|---|---|---|---|---|
| 1440 KB | MFM | 2 | 18 | `$02` (512-byte sectors) |
| 720 KB | MFM | 2 | 9 | `$02` |
| 800 KB | GCR | 2 | 12…8 by zone | `$22` |
| 400 KB | GCR | 1 | 12…8 by zone | `$02` |

Sector numbers on the media are 1-based in MFM and 0-based in GCR; the
hardware compares `FirstSector` against the raw header value.

### Writes and formats are the same parser

Both put a driver-built byte stream on the DMA channel in which `$99`
introduces a command: `99 0F` pass the next 512 bytes literally, `99 04`
write both CRC bytes, `99 08` end data, `99 A1`/`99 C2`/`99 FB`/`99 FE`
the missing-clock mark bytes, `99 99` a literal `$99`.  In GCR mode the
same escapes work, values below `$40` go through the hardware encode
table, and a byte with the high bit set is a literal pattern.  A write
parses one data field at the sector whose header matched; a format parses
a whole track image and takes each data field's sector number from the
address field that preceded it.

### Raw / copy-protect capture

With CopyProtMode set the chip streams two bytes per disk byte — a flag
(`$00`, or `$80` for a mark byte) then the data byte — from the next mark
byte until GO drops or the DMA count exhausts.  The engine synthesises
that stream from the sector model.

## Known gaps

- Write precompensation (`ParamData`) is accepted and ignored.
- Copy-protected media that check flux timing do not work, by design.
- Sense-change interrupts (`$10`) are never raised.
