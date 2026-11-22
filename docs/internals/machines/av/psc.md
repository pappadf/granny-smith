# PSC — Peripheral Subsystem Controller (343S1100)

The heart of the AV platform, and the reason this family cannot reuse the
Quadra substrate: the PSC absorbs VIA2, the whole system interrupt controller,
seven DMA channels, the Singer sound engine's register block and the DSP
reset latch. Implementation:
[src/machines/av/psc.c](../../../../src/machines/av/psc.c) /
[psc.h](../../../../src/machines/av/psc.h). Hardware reference:
[psc.md](../../../reference/machines/av/psc.md), cited by section below.

The VIA1 function the PSC *also* implements is not here — it is the generic
6522 model mapped at island offset 0.

## Interrupt controller

(Reference: [psc.md](../../../reference/machines/av/psc.md) §2.2 for the
device interrupt registers, §2.8 for the VIA windows, §3.1 for the interrupt
architecture.)

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

(Reference: [psc.md](../../../reference/machines/av/psc.md) §2.5–§2.6 for the
channel registers, §3.2 for the DMA engine, §5 for the quirks behind the table
below.)

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
| SWRESET (11) leaves the channel paused, clears ENABLED in **both** sets, and restarts it at set 0 with no completion pending (IF, TERMCNT clear) | teardown/recovery; the LocalTalk transmit spins until the word reads exactly `$C400` after each frame |
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
device pump can probe without touching its FIFO, and `av_psc_dma_remaining`
the active set's count — the New Age reads it to see terminal count on the
byte that ends a set, which is what ends its command (the second set may
already be armed for the next one).

**The SCC channels (4, 5, 6).** Writes to their registers call a second
hook (`av_psc_set_scc_touch_hook`), and av.c moves a transmit set (channel 5
for SCC B, 6 for SCC A) that is ready memory → device into the SCC's data
register at once, where the bytes join the SDLC frame as CPU writes would.
At once, because the ROM's LocalTalk driver (.MPP, `$40985070`–`$409851A0`)
hands each data frame (LLAP header included) to channel 5 and, right after
the un-pause, with interrupts masked, resets the Tx underrun/EOM latch and
turns the transmitter off: the frame must be in the SCC by then. It writes
only lapENQ and lapRTS by hand. Without this the AV Quadras sent no DDP
frame at all, and the Chooser listed nothing. Receive DMA (channel 4, port A
only) is not modelled: the port B LocalTalk driver reads by hand.

**The Ethernet channels (1, 2).** Writes to their registers call a third
hook (`av_psc_set_enet_touch_hook`), which wakes the Ethernet pump in
[enet.c](enet.md). Channel 2 (ENetWr) is an ordinary byte channel: one
frame per set, Cnt in bytes, the set's terminal count being the frame's
EOF into the MACE. Channel 1 (ENetRd) runs in **chain mode**
(`av_psc_dma_chain_in`), the receive model Apple's AV Ethernet driver
programs (`Mace.a` `InitRecv`/`PSCInterrupt`/`MaceRecv`; reference
[psc.md](../../../reference/machines/av/psc.md) §4.6):

- Addr is the physical base of a chain of 2048-byte buffers and **Cnt the
  number of buffers left**, not bytes — `InitRecv` writes the chain's Limit
  there and `MaceRecv` computes `In = Limit − Cnt`.
- One frame fills one buffer: the status block at the buffer's start (the
  four RCVFS bytes, each on both halves of a word), the frame from `+$10`
  (`RecvBuff`: `MaceStat` 8, fill 8, `Packet`). Addr then steps to the
  next buffer and Cnt drops by one.
- **IF is set per frame**, not only at terminal count: `PSCInterrupt`
  clears IF and IE on a set whose chain is not full and relies on the next
  frame (or the deferred task's IE re-enable) to interrupt again.
- The buffer that takes Cnt to 0 is the set's terminal count — the usual
  completion (ENABLED clears, TERMCNT, the active set flips) — and the next
  frame lands in the other set's chain.
- DIR is never written on either Ethernet channel; their direction is
  fixed, so the chain path does not check it.

**Not modelled:** FIFO depth and latency; DMAFLUSH completes at once
(nothing is ever in flight inside the PSC).

## Sound and DSP

(Reference: [psc.md](../../../reference/machines/av/psc.md) §2.7 for the
sound/DSP block, §3.5 for the sound DMA engine, §2.3 for the UTSC.)

The sound block is no longer a stub: the Singer frame engine
([singer.md](singer.md)) services the `$200`–`$21B` latches, and a live
DSP3210 sits behind the reset latch ([dsp.md](dsp.md)).

- **`sndPhase` (`$50F3120C`) must tick.** `CycloneBeep` runs on every boot at
  IPL 7 with caches off and spin-waits on the masked field twice (first for
  ≠ 0, then for = 0). A constant value hangs the ROM forever with no escape.
  Implemented as a free-running frame counter derived from emulated time at
  the programmed codec rate; the Singer frame engine is phase-locked to the
  same formula, so play position and frame interrupts cannot drift apart.
  **A word or long read is served from one sample.**  The access reaches the
  model as byte reads with bus time passing between them; sampling each lane
  afresh tore the value whenever the offset carried between lanes (at
  1023 → 1024 a long read returned `$00000000`), and `CycloneBeep`, which
  spins for offset 0 and then chains the next double buffer, chained twice
  in that pass, skipping chime audio and ending it early.
- **`dspOverRun` (`$50F3121C`)** is a sense-bit latch over
  pdspReset/pdspResetEn/pdspFrameOvr; writes are surfaced to the DSP glue
  (`av_psc_set_dsp_hook`) — a bit-0 clear write is the RTM's release, and on
  a healthy boot the kernel handshake succeeds with no `$81` timeout.  L5
  bit 1 (FRMOVRN) is a level view of the sticky `pdspFrameOvr` bit.
- The `$200`–`$21B` latches read back and feed the frame engine
  (`av_psc_snd_read16/32`); `singerStat` reads the board-strap presentation
  `AV_SINGER_STAT` (mic present, [singer.md](singer.md)).
- **UTSC** (`$300`/`$304`) is a monotonic 48-bit counter. The real tick source
  is undocumented; the serial HAL only uses bits 16..47 as a multi-millisecond
  timeout reference, so ~1 MHz is a safe choice.

## Checkpoint

The PSC saves an explicit field list (`AV_PSC_CP_FIELDS` /
`AV_PSC_CHAN_CP_FIELDS`, checkpoint.h "Field lists"): the VIA2 window and
L3–L6 files, the sound/DSP latches, and per channel both sets' Addr, Cnt
and CmdStat, the active set and the PAUSE/CIE/BERR/SENSE bits. Host
pointers (hooks, the memory port, the object node) are rebound at
construction, never saved.

## Testing

`tests/unit/suites/psc/` replays the known-good client sequences the
shipping drivers issue: the SCSI HAL's
`StartPSC`/`PausePSC`/`StopPSCRead`, `MaceInit`'s interrupt gating plus
`ResetMACE`'s SWRESET, the New Age driver's both-sets dance across the
automatic set switch, the Ethernet driver's chain-mode receive
(`InitRecv`, `PSCInterrupt`'s IF/IE handling, the chain-full switch) and
double-buffered transmit (`PrimeXmitRS`) — plus the direction gate, the
≥`$40000000` restriction, the Ethernet touch hook and a checkpoint round
trip of the field list.
