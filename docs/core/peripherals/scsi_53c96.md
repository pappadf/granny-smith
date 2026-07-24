# NCR 53C96 SCSI Controller (+ TurboSCSI pseudo-DMA)

The **NCR 53C96** Advanced SCSI Controller is the Quadra generation's
SCSI chip, implemented machine-independently in
[src/core/peripherals/scsi_53c96.c](../../../src/core/peripherals/scsi_53c96.c) /
[scsi_53c96.h](../../../src/core/peripherals/scsi_53c96.h). It is the
protocol front-end over the shared bus/target/CD-ROM model in
[scsi.c](../../../src/core/peripherals/scsi.c), reached through the
bus's external-initiator API (`scsi_external_*`) — there is no NCR 5380
register file on this family, and the 5380 path is untouched.

Ground truth: the NCR 53C94/95/96 Data Manual (register semantics,
reset/interrupt behaviour) plus the boot ROM's and System 7.1 SCSI
Manager's observed command flows.

## Chip model

- Full register file with data-manual-faithful reset and interrupt
  semantics: chip reset, NOP, flush FIFO, SCSI bus reset,
  enable/disable selection, select sequences (with/without ATN, DMA
  variants), Transfer Information, ICCS, message-accepted.
- Selection time-outs scale from the register value ×8192 ×
  clock-conversion / clock (a scheduler event).
- INT is a level output (VIA2 CB2 on the Quadras; the towers wire-OR two
  controllers). `scsi_53c96_set_irq_callback` re-drives the current level
  on (re)bind so checkpoint restore re-establishes the VIA input.

## TurboSCSI pseudo-DMA

The pseudo-DMA aperture (island `$F100` / `$F502`) is **not
bus-mastering**: the CPU moves 16-bit words (or bytes) through the port
while a DMA Transfer Information command is active; DAFB only supplies
the DRQ-gated acknowledge timing, which the functional model satisfies
immediately (`scsi_53c96_dreq` feeds DAFB's control-register bit 9
readback). The transfer counter is lazy/counter-driven — there is no
holding buffer.

## Behaviours the Mac software pins

Four subtleties, each unit-pinned in `tests/unit/suites/scsi96/` and
found the hard way during the System 7.1 HD-boot bring-up:

1. **Odd-length FIFO residual.** The chip reserves the trailing byte of
   an odd transfer in its FIFO: the completion interrupt fires when the
   counter reaches `count & 1`, and the last byte is read from the FIFO
   register — not the aperture. (The ROM's Duff's-device drain reads 255
   words through the aperture, waits for INT, then `MOVE.B` from the
   FIFO.)
2. **Command-phase Transfer Information.** The SCSI Manager selects
   without ATN, then issues non-DMA Transfer Info in COMMAND phase to
   feed the CDB from the FIFO.
3. **Live-phase status register.** The status register's low 3 bits are
   combinational from the bus phase lines — refreshed on every read; the
   SCSI Manager polls for COMMAND phase without issuing another command.
4. **Flush FIFO does not abandon a paused DMA select.** The System's HD
   driver pauses a DMA-select in COMMAND phase, flushes the FIFO, then
   pushes the CDB byte-wise through the pseudo-DMA aperture. Flush
   clears the FIFO only — inventing an `xfer_mode` reset here silently
   dropped CDBs and surfaced hundreds of millions of instructions later
   as a CommToolbox null-dispatch crash.

## State

`scsi_53c96_checkpoint` serializes the whole chip struct; restore NULLs
the pointer/callback fields for rebinding and (for now) resets
`xfer_mode` to idle — a checkpoint saved mid-pseudo-DMA-burst resumes at
the transaction boundary. Machine wiring: [q700](../../machines/mcu/q700.md)
one controller, [q900/q950](../../machines/mcu/q900.md) dual.
