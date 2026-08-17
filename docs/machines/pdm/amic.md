# AMIC — the PDM I/O controller

`src/machines/pdm/amic.c`.  AMIC implements essentially all PDM I/O
logic: the whole classic-Mac interrupt model, a 10-channel DMA engine, the
AWACS sound engine, built-in video timing, and all `$50Fxxxxx` address
decode.  Sources: Apple, *Power Macintosh Computers* Developer Note (1994)
Fig 2-2 and pp. 15–23, the 8100 schematics, and the shipping ROM's
hardware-init writes.  Phase C models the full software-visible register
surface; DMA datapaths, audio rendering, and video scanout land with their
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
  With INTMODE=1 (the shipping state) any assertion latches CPUINT; a
  write with bit 7 set acks the latch, and a still-pending source
  re-latches immediately — the level-style model the ROM's
  loop-until-clear dispatch requires ("a pure edge model that drops
  interrupts while masked will hang drivers").
- Pseudo-VIA2 slot IFR reads **active-low** with unused bits high (reset
  `$7F`); slot bits are live card levels; VBL (bit 6) is the only
  software-clearable bit (write `$40`).  The device bank aggregates
  SCSI/FDC/any-slot levels gated by set/clear-convention IERs (writable
  masks `$78` slot / `$3B` device).
- DMA flag registers `+8`/`+$A` are read-only mirrors of the per-channel
  IF∧IE bits; acks go to the channel's own control byte (IF is
  write-1-to-clear).

## Sound engine (Phase-C subset)

While the output-run bit (`+$14010` bit 0) is set, buffers complete on the
real ping-pong cadence (BufferSize frames at 22 050/29 400/44 100 Hz per
the rate field) and raise their done flags in `+$14018` — bit 6 pairs with
the `+$10000` window buffer, bit 7 with `+$12000`; a still-set flag raises
ERR (bit 5) instead.  This is the polled contract the PPC ROM's boot chime
depends on (it runs with interrupts off — a frozen engine hangs boot).
Flags are write-1-to-clear; enables live in the low nibble.

## DMA register file (Phase-C subset)

The register surface stores and reads back with the documented semantics
(RST self-clears and stops a channel; SCSI keeps DIR and the bus-speed
field — bits 3:2 of `$50F32008` must stick, the ROM's AMIC-revision probe
depends on it; SCSI FLUSH self-clears; the floppy channel's address
resets to `$15000`; Enet Tx SET0/SET1 read 1 = "buffer free").  No data
moves yet — transfer engines arrive with their devices.
