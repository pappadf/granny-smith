# AMIC — the PDM I/O controller

`src/machines/pdm/amic.c`.  AMIC implements essentially all PDM I/O
logic: the whole classic-Mac interrupt model, a 10-channel DMA engine, the
AWACS sound engine, built-in video timing, and all `$50Fxxxxx` address
decode.  Sources: Apple, *Power Macintosh Computers* Developer Note (1994)
Fig 2-2 and pp. 15–23, the 8100 schematics, and the shipping ROM's
hardware-init writes.  The sound block dispatches to `awacs.c`
(`awacs.md`), video control and the Ariel CLUT to `ariel.c` (`video.md`);
remaining DMA datapaths (SCSI, floppy, SCC, Ethernet) land with their
ladder rungs (proposal-powerpc-601-pdm.md §6).

## Decode (island offsets from `$50F00000`)

| Offset | Block |
|---|---|
| `+$00000` | pseudo-VIA1 — a real 6522 core instance, register *n* at `n×$200` (IFR `$50F01A00`, IER `$50F01C00`) |
| `+$14000` | sound block ($20 bytes: codec control/status, buffer size, phase, run bits, in/out IRQ status) |
| `+$24000` | Ariel II CLUT/DAC (address/data/control/key; data auto-advances the RGB phase) |
| `+$26000` | pseudo-VIA2 bank: slot IFR `+2`, device IFR `+3`, slot IER `+$12`, device IER `+$13` |
| `+$28000` | video control: mode (`$9F` = blanked at reset), depth, monitor sense, vtest, beam counters |
| `+$2A000` | interrupt control register (ICR) + DMA flag mirrors at `+8`/`+$A` |
| `+$2C000` | diagnostic register — **bit 0 must read 1** or boot detours into the ROM serial monitor |
| `+$31000` | DMA register file (window base, per-channel address/count/control through `+$122xx`) |
| `+$40000` | the HMC serial config port (hmc.md) |

All registers are byte-wide; wider accesses decompose big-endian.

## Interrupt model

One level-sensitive wire into the 601 (`ppc_set_ext_irq`), recomputed
after **every** flag/enable write:

- ICR (`$50F2A000`): bits 5..0 are live source levels (NMI, DMA, MACE,
  SCC, pseudo-VIA2, pseudo-VIA1); bit 6 INTMODE; bit 7 the CPUINT latch.
  With INTMODE=1 (the shipping state) the latch is **"interrupt on
  change"**: any CHANGE of the source picture — assertion OR deassertion
  — sets CPUINT; a write with bit 7 set acks it; toggling INTMODE clears
  it.  A source that merely stays asserted does NOT re-latch after the
  ack.  Both halves are load-bearing: assertion-only re-latching would
  livelock the early 68k boot (which runs at IPL 7 with a pending VIA
  source the kernel keeps redelivering), and WITHOUT deassertion latching
  the nanokernel never re-reads the flags, never sees 0, and never clears
  the 68k emulator's posted interrupt level — the emulator then redelivers
  the stale level forever.  The 68k dispatcher's "no source" jump-table
  entry exists precisely for the deassertion-change interrupts.  The
  source summary bits stay live levels, which is where the dossier's
  "a pure edge model that drops interrupts while masked will hang
  drivers" warning applies — the 68k handlers loop on the flag registers.
- Pseudo-VIA2 slot IFR reads **active-low** with unused bits high (reset
  `$7F`); slot bits are live card levels (`bit = slot - 9`) — bit 2 = the
  decoded-but-unpopulated `$B`, bits 3/4/5 = the connectors `$C`/`$D`/`$E`
  (the set a booted OS enables), driven from the bus controller
  through `pdm_amic_set_slot_irq` (each connector's `/NMRQ` runs to an AMIC
  pin; the bridge is not in that path — see bart.md); VBL (bit 6) is the only
  software-clearable bit (write `$40`).  The device bank aggregates
  SCSI/FDC/any-slot levels gated by set/clear-convention IERs (writable
  masks `$78` slot / `$3B` device).
- **VBL (Phase E)**: a free-running raster event asserts the slot IFR VBL
  flag (drives bit 6 LOW) every frame at 66⅔ Hz — the Hi-Res 640×480
  mode's field rate, an exact cycle count (freq×3/200) on every PDM
  clock.  This resolves the dossier's §11.6 polarity suspect empirically:
  the ROM's `SonoraWaitVSync` clears the flag with a `$40` write and then
  spins until bit 6 **reads 0**, so assertion is active-low — and without
  the raster that spin is exactly where the boot hangs.  The enable bit
  (`SonoraSlotIER` bit 6) gates only the interrupt, never the flag.

## Monitor sense (Phase E)

`SonoraVdSenseRg` (`+$28002`) models the three open-collector HDI-45
sense lines with 10 kΩ pull-ups: readback bits 6:4 = wired-AND of the
drive nibble (bit *n* = 0 drives line *n* low; `$07` = tristate) and the
monitor's straps.  The emulated monitor is the **14" AppleColor Hi-Res**
(sense code 6: A,B floating, C grounded), chosen because `HMCMerge`
allocates the framebuffer window only when a monitor senses present
(rung L18) and it is the Phase-F gray-desktop profile.  The extended-
sense walk over a strap-only monitor yields the non-Multiple-Scan code,
so the ROM falls back to the plain Hi-Res configuration — the real
hardware behaviour for this monitor.
- DMA flag registers `+8`/`+$A` are read-only **combinational** mirrors of
  the per-engine flag∧enable bits (the sound engine's `+$14014`/`+$14018`
  for `+$A`, the channel control bytes for `+8`); acks go to the engine's
  own register — the ROM never writes the mirrors — and any set mirror
  bit asserts the ICR's DMA source level.

## DMA register file (Phase-C subset)

The register surface stores and reads back with the documented semantics
(RST self-clears and stops a channel; SCSI keeps DIR and the bus-speed
field — bits 3:2 of `$50F32008` must stick, the ROM's AMIC-revision probe
depends on it; SCSI FLUSH self-clears; the floppy channel's address
resets to `$15000`; Enet Tx SET0/SET1 read 1 = "buffer free").  The
transfer engines live with their devices — SCSI and floppy in their own
files, the SCC serial engines below.

## SCC serial DMA engines

Register blocks `$50F32080` / `$50F32090` / `$50F320A0` / `$50F320B0`
(TxA / RxA / TxB / RxB) carry a 32-bit address, a 13-bit count and the
control byte; each channel owns an 8 KB ring inside the DMA window.  The
guest arms a channel by writing RST, the count and RUN, then polls DMAIF
(bit 7) — Mac OS 8.1's SerialDMA HAL and the PDM `.MPP` LocalTalk driver
both use IF with IE clear, so the completion flag, not an interrupt, is
what ends a transfer.

- **Transmit** is one-shot: at terminal count the engine hands the ring's
  bytes to the ESCC write path, flushes the SDLC frame (Tx underrun/EOM)
  and sets IF.  It is delivered after the wire time of `count` bytes —
  35 µs each, one SDLC byte at LocalTalk's 230.4 kb/s.
- **Receive** is the LAP driver's "ReadRest": it reads a frame's 5-byte
  header through the ESCC by hand, then hands the remainder to the engine,
  which drains that many bytes out of the receive FIFO into the ring (the
  trailing CRC stays in the FIFO for the driver to pop) and sets IF.

**Only bytes that have not arrived yet cost wire time.**  The LLAP
transport hands a frame to the ESCC whole and has already paced it onto
the wire (`appletalk.c` holds the next RTS for 35 µs/byte plus the
inter-frame gap), so a receive channel is typically armed with its whole
remainder already sitting in the FIFO; the arm charges wire time only for
the shortfall, and a DMA-burst latency otherwise.  Charging the frame's
wire time a second time stalls the engine for a full frame — and the 8.1
LocalTalk driver polls DMAIF from its `CheckDMA` loop **at IPL 1**, so
each of those stalls is ~20 ms with VIA1 interrupt service starved: `Ticks`
stop, and a Cuda autopoll packet landing inside the blackout desynchronizes
the ADB byte stream and wedges the guest (#124).
