# New Age — the AV floppy controller model (`new_age.c`)

New Age is the floppy controller of the Quadra 840AV and Centris 660AV: an
NEC µPD72070 strapped into Apple mode, driving one internal SuperDrive. It
is a 765-family part — a command/result protocol through a status register
and a data register — and nothing like the IWM/SWIM/SWIM3 lineage. The chip
model is board-independent; the AV supplies a small backend:

| Part | File | What it owns |
|---|---|---|
| chip | `src/core/peripherals/new_age.c` | the three-phase host protocol, the Apple-mode command set, the drive command latches (enable, motor, GCR/MFM mode), the sector-level transfer engine, the /CSTIN poller, the rotational model |
| drive and media | `src/core/peripherals/floppy.c` (`FLOPPY_TYPE_NEW_AGE`) | head position, motor, the disk image, `machine.floppy.*` |
| board backend | `src/machines/av/new_age.c` | the `$50F2A000` decode, the PSC channel-3 byte movers with terminal count, the PSC-VIA2 bit-5 interrupt |

The hardware is described in the reference page
[new-age.md](../../../reference/machines/av/new-age.md) (§2 registers, §3
behaviour, §4 the shipped driver's sequences); this document covers the
emulator model. Where the shipped driver and the specification disagree,
the model follows the driver, and says so at the site.

## The backend contract (`new_age_backend_t`)

```c
int  (*dma_put)(void *ctx, uint8_t value);  // NEW_AGE_DMA_NONE / _OK / _TC
int  (*dma_get)(void *ctx, uint8_t *out);   // the same, memory -> device
void (*set_irq)(void *ctx, bool level);     // the INT pin
```

Each mover moves one byte and reports whether it did, and whether that byte
carried **terminal count**. TC is the board's: the chip ends a transfer
normally on TC, and without it runs to EOT and ends abnormally with
ST0 = $40 / ST1 = $80. On the AV the PSC terminates a register set's count
on its last byte, so the backend reads `av_psc_dma_remaining()` before
moving the byte. This has to be per set, not "the channel went idle": the
driver's whole-track read (`MFMTrack`) arms **both** PSC register sets
before it launches the first of two Read Data commands — the first set
must deliver TC at the end of the first command while the second set is
already armed for the next.

`new_age_t` is plain data first — the board checkpoints it positionally up
to `fd` — followed by the drive, scheduler and backend pointers, re-bound
with `new_age_bind()` after a restore.

## The host protocol

Two registers: STR (read) / DRR (write) and the data register. The board
maps them as register 0 and 1 (`NEW_AGE_REG_STATUS`, `NEW_AGE_REG_DATA`).

| Phase | STR | Notes |
|---|---|---|
| idle | RQM | |
| command | RQM, CB | one byte per write; the last ("launch") byte starts the command |
| execution | CB | data commands only: RQM stays low until the result phase |
| result | RQM, DIO, CB | one byte per read; the first read drops INT |

The execution phase's RQM-low matters. The driver's `WriteToFDC` waits for
RQM after every byte it writes, so for the launch byte it is waiting for
the transfer to finish; its DMA-done poll that follows (a ~4 ms
TimeDBRA loop) then runs against a command that is already in its result
phase. A model that left RQM up during execution would put the whole
transfer inside that short poll.

The drive commands — Seek, Recalibrate, Set Motor Control on, Set Drive
Mode — run in the background like a 765 seek: the chip is idle again at
once and interrupts when the drive's handshake ends. Set Enable Control,
Set Motor Control off, Eject and the DPLL commands interrupt at once.
Sense Interrupt Status collects the one pending cause (two bytes after a
seek-family end, one otherwise, $80 with nothing pending); Sense Drive
Status and Revision answer without interrupting.

The interrupt is a level. Command arrival and the first result read drop
it; nothing the host writes to the PSC's IFR does. The driver's
interrupt-status routine waits for CB to be **clear** after the flag
appears, and its result routine waits for CB to be **set** — the idle and
result-phase values above.

## Drive and media

ST3 is built from the drive: `$FF` for a drive the machine does not have
(the driver's drive-absent test at open), and for the SuperDrive bit 3
(2 MB class) and bit 2 (not a Typhoon) set, with media density, write
protection, /Ready, /TK0 and the drive's own GCR/MFM latch in the rest. An
empty drive reads write-protected and not ready.

A command reads the medium only if the opcode's GCR/MFM bit, the drive's
mode latch and the medium's recording agree, and — for MFM — the DRR
selects 500 kbps with conventional (not 1 Mbps perpendicular) recording.
Anything else finds no address mark after the search timeout (400 ms in
GCR, two revolutions in MFM), which is how the driver's GCR-then-MFM probe
ladder (`SetUpFDC`) tells the formats apart.

Sectors move as 512-byte blocks between the image and the DMA channel; the
chip's GCR/MFM encoding is not synthesised (Apple's driver never asks for
tag bytes, so nothing else crosses the bus). Raw Dump is not modelled and
ends with no address mark.

**Rotation** follows emulated time at the drive's speed — 300 rpm for
1.44 MB, 600 rpm for 720 KB (both at 500 kbps), the zone speed in GCR —
because the driver observes it twice: `MFMTrack` starts its whole-track
read at the header it just saw, and the MFM probe walks headers with Read
ID until the sector number wraps. Read ID returns the next header to pass;
Read/Write Data wait for the first sector to come round, then take one
sector time each. The model's track has no interleave; nothing the driver
does observes the physical order.

**Formatting** consumes the four-byte C/H/R/N entry per sector over DMA
(Format/Write adds each sector's 512 data bytes) and lays down the sectors
whose numbers fit the track. Apple's `<LW7>` GCR erase pass sends $3F for
every sector number and so writes nothing readable. The format byte (GCR)
or sector count (MFM) records the medium's new format with
`floppy_media_set_format()`.

## Media change: the /CSTIN poller

The firmware's idle loop compares each drive's /CSTIN with the level it
last reported and interrupts with IC = %11 on a change, then holds off
100 ms. The model polls every 10 ms, only while the chip is idle, no drive
is enabled and no interrupt waits to be collected, and not within 5 ms of
the host's last command byte (so it cannot land between the bytes of a
sequence the host is in the middle of).

A reset forgets the remembered levels. That is what tells the driver
about a disk that is already in the drive when it opens: its open
sequence ends with every drive disabled, the poller then reports the disk
as an insertion, and the driver's interrupt handler posts the disk-insert
event that mounts (or boots from) it. While a disk is mounted the driver
keeps its drive enabled, so polling is suspended until the eject; Eject
interrupts at once, and the medium leaves the drive half a second later,
which the poller reports as the second interrupt.

**A host-side eject is invisible to the guest.** `machine.floppy.drive[N].eject`
takes the medium out of the drive directly, as on every model, but on this
machine nothing tells the driver: its drive is still enabled, so the poller
is suspended, and it goes on believing the disk is in place — and a disk
inserted after that is not reported either, until the guest ejects (or the
machine resets). This is the hardware's behaviour (the AV's SuperDrive is
manual-inject, auto-eject, and the chip cannot poll an enabled drive); eject
from the guest — Put Away, or dragging the disk to the Trash.

## Deviations from the specification

- **The result C at EOT.** The specification's ID-update table advances C
  when a MT = 0 transfer ends at EOT. The model does not: the driver keeps
  the result's C as the head's cylinder and skips the next seek when it
  matches, and every whole-track read ends at EOT — with C+1 the next
  transfer lands on the wrong cylinder and only recovers through retries
  and a recalibrate (seen as a format crawling at one track per 25
  retries before the fix). R still steps and wraps as the table says.
- **Timing** is scaled where a real value would only slow the emulator: the
  drive mode change answers in 50 ms (800 ms max), the eject in 500 ms
  (1.5 s max). Motor spin-up (400 ms), step and settle times and the mark
  searches are realistic; every driver wait is a TimeDBRA loop several
  times longer than its nominal figure.

## Testing

`tests/integration/suite-av/`: `av-floppy-mount` (1.44 MB MFM on the
840AV), `av-floppy-800k` (GCR on the 660AV), `av-floppy-write-eject`,
`av-floppy-format` (a blank disk initialized from the Finder) and
`av-floppy-boot` (the 7.6 install floppy boots the 840AV with no SCSI
disk). `machine-capabilities` pins the one internal SuperDrive in both
profiles.
