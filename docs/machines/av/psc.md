# PSC — Peripheral Subsystem Controller (343S1100)

The heart of the AV platform, and the reason this family cannot reuse the
Quadra substrate: the PSC absorbs VIA2, the whole system interrupt controller,
seven DMA channels, the Singer sound engine's register block and the DSP
reset latch. Implementation:
[src/machines/av/psc.c](../../../src/machines/av/psc.c) /
[psc.h](../../../src/machines/av/psc.h).

The VIA1 function the PSC *also* implements is not here — it is the generic
6522 model mapped at island offset 0.

## Interrupt controller

Three surfaces, all repeat-read stable (every ROM dispatcher reads twice and
loops until two reads agree — a deterministic model satisfies this trivially,
but **no read may have side effects**):

- **The pseudo-VIA2 window** at `$50F02000`: only `$1A00` IFR, `$1C00` IER and
  `$1E00` SInt exist. IER writes use the VIA sense-bit convention
  (`$80|bits` sets, `bits` clears); IFR is write-1-to-clear for latched bits
  while level sources re-derive. SInt reads the slot lines **active low**.
  Sources: SCSI on bit 3, the slot/VBL aggregate on bit 1, FDC on bit 5,
  sound frame on bit 6. This window owns IPL 2.
- **Level registers L3–L6** (`$130`/`$140`/`$150`/`$160` IR + `+$4` IER),
  byte-wide, sense-bit enables, IR write-back-to-clear for latched bits.
  Bit 7 of an IR reads as the OR of everything pending on that level.
- **`PSC_ISR`** at `$804`: bit `31−n` = channel *n* interrupting, in the bit
  order the ROM's `BFFFO D5{0:8}` dispatch expects, gated by the channel's
  CIE and the completing set's IE. It feeds the L4 DMA bit (3).

The FDC bit is modelled as a **level**, not a latch: the New Age deasserts its
INT when the host reads the interrupt status, and the driver's handler never
writes the IFR to acknowledge. A latched model re-enters the level-2 handler
forever.

## DMA engine

Seven channels (0 SCSI, 1/2 MACE rx/tx, 3 FDC, 4/6 SCC A rx/tx, 5 SCC B);
sound is a separate engine, not a channel. Per channel: a word control
register at `$C00 + n*$10` and **two** {Addr, Cnt, CmdStat} register sets at
`$1000 + n*$20` (set 1 at `+$10`).

The behaviors that hang a driver if wrong, all implemented and unit-tested:

| Behavior | Why it matters |
|---|---|
| Control bit 0 = active set | drivers read it and index the sets accordingly |
| FROZEN (14) asserts after PAUSE | `PausePSC` spins forever otherwise |
| DMAFLUSH (9) self-clears | clients poll until it reads 0 |
| SWRESET (11) leaves the channel paused, clears ENABLED in **both** sets | teardown/recovery |
| Sense bit (15) on control and CmdStat writes | enables and arms silently invert without it |
| Completion: ENABLED clears, TERMCNT + IF set, **active set flips** | gapless double-buffered streaming |
| Cnt holds the residual and is writable | `StopPSCRead` zeroes it in a read-verify loop |
| DIR = 1 means device→memory | direction gate on the transfer ports |
| No DMA to addresses ≥ `$40000000` | Radar #1059322; latches BERR instead |

The SENSE bit also **reads back latched** in the control word. That is not
cosmetic: the ROM's serial HAL writes `$8800` (SENSE|SWRESET) and then
compares the whole word against `$C400`, so a model that drops the bit spins
there forever.

Transfers run through guest-physical memory hooks (the `sonic_set_memory_hooks`
pattern — the CPU MMU is deliberately not in the path). Devices drive
`av_psc_dma_device_in/out`; `av_psc_dma_dir` reports the armed direction so a
device pump can probe without touching its FIFO.

**Not modelled:** the MACE receive channel's "chain mode" (where Cnt counts
buffers rather than bytes), and FIFO depth/latency — nothing in scope needs
either.

## Sound and DSP stubs

- **`sndPhase` (`$50F3120C`) must tick.** `CycloneBeep` runs on every boot at
  IPL 7 with caches off and spin-waits on the masked field twice (first for
  ≠ 0, then for = 0). A constant value hangs the ROM forever with no escape.
  Implemented as a free-running frame counter derived from emulated time at
  the programmed codec rate.
- **`dspOverRun` (`$50F3121C`)** is a sense-bit latch over
  pdspReset/pdspResetEn/pdspFrameOvr with no DSP behind it. On a real boot the
  enabler's Real Time Manager writes `$01` (release reset), polls its
  handshake cell, times out and writes `$81` (re-assert) — the documented
  graceful failure, verified end-to-end.
- The rest of the `$200`–`$21C` block latches and reads back; `singerStat`
  reading 0 is the datasheet-correct idle value.
- **UTSC** (`$300`/`$304`) is a monotonic 48-bit counter. The real tick source
  is undocumented; the serial HAL only uses bits 16..47 as a multi-millisecond
  timeout reference, so ~1 MHz is a safe choice.

## Testing

`tests/unit/suites/psc/` replays the three known-good client sequences the
shipping drivers issue: the SCSI HAL's
`StartPSC`/`PausePSC`/`StopPSCRead`, `MaceInit`'s interrupt gating plus
`ResetMACE`'s SWRESET, and the New Age driver's both-sets dance across the
automatic set switch — plus the direction gate and the ≥`$40000000`
restriction.
