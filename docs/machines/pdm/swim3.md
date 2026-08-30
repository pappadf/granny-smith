# SWIM III on the PDM — the AMIC side of the floppy datapath

The chip itself — registers, drive protocol, transfer engine — is the
shared model in `src/core/peripherals/swim3.c` / `swim3_xfer.c`,
documented in [docs/core/peripherals/swim3.md](../../core/peripherals/swim3.md).
This page is what the 6100/7100/8100 add around it: where the AMIC island
decodes it, how the AMIC floppy DMA channel feeds it, and where its
interrupt lands.  `src/machines/pdm/swim3.c` is the board's face of the
shared model — the three DMA movers and the interrupt sink it is bound
to — and `amic.c` owns the channel.

All three models have the same subsystem — one internal manual-inject
SuperDrive, no external port — so this is one piece of hardware for the
whole family.

Sources beyond the shared page's: the Power Macintosh 8100 schematic set
(051-0333 rev A, sheets 12 and 20).

## Why the chip must exist even with no floppy

Worth stating before anything else, because it cost a debugging session
during the family's bring-up: **SWIM3 is load-bearing for any System boot,
including a SCSI boot with no floppy media at all.**  With `$50F16000`
unwired, the ROM `.Sony` driver's open fails its presence probe, tears
down, and leaves `SonyVars` (`$134`) = −1 — and System 7.5 later
dereferences `SonyVars−$C` unconditionally, producing a wild jump and a
bomb a long way from the cause.  (The same is true of the TNT family and
the Network Servers, which is why the shared model exists.)

## Decoding

Sixteen byte-wide registers at `$50F16000`, **stride `$200`** (index =
offset >> 9; the chip's A0–A3 hang off `BufAddr<9..12>`).  So Data is at
`$0000`, Timer at `$0200`, … IntMask at `$1E00`.  Presenting IWM
behaviour at `$50F16000` would be wrong: there is no IWM mode and no ISM
compatibility in this part.

The Mac OS `.Sony` driver's gates, all met by the shared model: Phase
reads back (`InitSWIMChip`'s `$05`/`$06`/`$07` presence loop — a
mismatch is `initIWMErr` and the `SonyVars` bomb above), Setup reads
back (`LoadSWIMparams` compares), IntMask is `bset`/`bclr`'d, Interrupt
and Error clear on read.  Two ordering rules the driver relies on:
WriteMode is only ever set while GO is clear, and stopping a format
clears GO **first**, then FormatMode.

The interrupt line is **pseudo-VIA2 device bit 5**, dispatched at 68k
level 2 — *not* the AMIC DMA interrupt.  Which of the two paths a
transfer ends on matters (below).

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

The three movers the shared engine is bound to (`pdm_amic_fd_dma_running`
/ `_get` / `_put`) are the channel's RUN/direction state and one byte in
either direction; the channel does the address advance, the count and the
completion interrupt.

## What the driver does that the shared page describes generically

- The GCR format routine **self-tunes its intersector sync count** by
  measuring rotational wrap with `_GetMicroSeconds`; an emulator whose
  format timing is wildly fast makes that loop shrink the gap down to
  `GCRMinSyncCount` and fail with `fmt1Err`/`fmt2Err` — which is why the
  engine models rotation.
- Its timeouts are 300 ms per track operation, ~8 ms of sector data
  time, 1 s minimum per track request; the wrong-mode attempts in the
  format search (MFM1440K, MFM720K, GCR800K, GCR400K, GCRonHD) fail on
  the 300 ms one, because the engine presents no address fields in the
  wrong framing.  This also covers the `<SM16>` pitfall — reading an MFM
  header in GCR mode must not error, but must not look valid either.
- Its sync group `3F BF 1E 34 3C 3F` is the spec-sanctioned spelling of
  the canonical `FF 3F CF F3 FC FF` (high-bit bytes are literal patterns
  in the GCR write stream).
- It samples a sense read at **Handshake bit 2**; the shared model also
  answers on bit 3 for the drivers that read it there.
- It never touches the Timer (it uses the Time Manager).

## Machine seams

`pdm_init` creates the drive with `floppy_init(FLOPPY_TYPE_SWIM3, NULL,
…)` — the NULL map is deliberate: PDM decodes SWIM3 through the AMIC
island, not through a memory-mapped floppy region, so the shared module
carries only the drive and the media.  `pdm_fd_insert` / `pdm_fd_present`
forward to it for drive 0 and refuse drive 1, which does not exist on this
family.

The chip state is the `swim3` member of `pdm_amic_t`, checkpointed
positionally with the island; `pdm_swim3_bind` re-attaches the drive,
scheduler and backend after a restore.  The floppy block is appended
after the SCSI blocks and before the HMC/AMIC tail, and PDM also saves
the image list (`mac_checkpoint_save_images`) — without it a restore had
nothing to resolve media filenames against, and the SCSI restore
dereferenced a null image.

## Known gaps

- The AMIC floppy channel's "flush" bit (control bit 4) is a latch; its
  floppy-side behaviour is undocumented and the driver does not use it.
- The shared page's gaps (precompensation, flux-timed copy protection,
  sense-change interrupts) apply here too.
