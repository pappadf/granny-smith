# AV DSP3210 integration (`src/machines/av/dsp.c`)

The AT&T DSP3210 — the AV family's floating-point DSP — as a live
auxiliary core.  The generic core lives in `src/core/cpu/dsp3210/`
(adapted from the validated dossier core, see the header of
`dsp3210.h`); this file binds it to the board.  Contracts:
the AV DSP3210 hardware notes §8 and the PlainTalk gap-closure findings.

## Execution

Burst events per the cores.md model: `ratio_x256` from the profile's
`aux_cpus[0].freq` (66.6667 MHz Cyclone / 55.5 MHz Tempest, 4 CKI per
instruction) over the main clock; quantum 4096 main cycles (~102 µs at
40 MHz).  A parked core (reset, or waiti with nothing pending and the
on-chip timer stopped) has no event; wake paths (`reset release, EXT1
tick, timer`) re-arm at +1 cycle and `cpu_reschedule()`.

## Board wiring

- **Bus hooks** — guest-physical through the bus resolver (`g_mmu`), the
  PSC-DMA pattern; the CPU MMU is deliberately not in the path.  The
  host decoder never maps the on-chip `$5003xxxx` window (the core
  decodes it internally).  One IO alias: DSP `$F0000000-$F07FFFFF` →
  host `$50800000 | (addr & $7FFFFF)` — the driver's `'phas'` selector
  hands the kernel the PSC sndPhase register as `$F073120C` and the
  sound team phase-syncs through it every frame.  Everything else at
  `$40000000`+ faults → DSP bus-error vector 1 → Apple's own `'xbus'`
  crash dump.  In particular there is **no** device window at
  `$50040000`: that address is only the kernel's hmem heap ceiling
  (kernel data `+$1A8`/`+$1F8`), all FIFO rings live in host RAM via
  `DSPFIFO` records, and anything that lands there is a runaway
  pointer that should fault (see
  the DSP3210 errata E10 for the bug that once made it
  look like a sound-FIFO window).
- **Reset lifecycle** — the PSC `dspOverRun` ($21C) hook: a bit-0 CLEAR
  write releases the DSP (the power-on release finds the latch already
  reading 0 — hardware reset held the chip until then) and starts
  execution at external physical 0, where the RTM's 7-word bootstrap
  waits; a bit-0 SET write ($83/$81) re-holds with a full state clear.
- **DSP→host doorbell** — the kernel's per-message BIO0 toggle (the
  16-bit `bio` write op `%11` on field 0) surfaces through the core's
  BIO callback; the glue latches **PSC L5 bit 0**.  The RTM's `DSPhndlr`
  acks L5IR itself.  The output register is watched regardless of
  `bioc` (gap-closure B1).
- **Frame tick in** — the Singer engine pulses **EXT1 (vector 15)** once
  per sound frame while `pFrmIntEn` is set (see singer.md); the on-chip
  timer (vector 9) is the kernel's steady-state heartbeat.  The pulse is
  a **4-instruction-slot active-low pin assertion**
  (`av_dsp_ext1_tick`, `AV_DSP_EXT1_PULSE_SLOTS`): PS.IR0/IR1 mirror
  the **live pin level** (1 = negated), which the kernel's boot
  calibration gadget and overrun poll both spin on
  (dsp-kernel-messages.md §3.2/§3.4).  The width is load-bearing in
  both directions — it must outlast the gadget's 2-slot spin but expire
  before its next check ~9 slots later; a 48-slot pulse reproduces the
  original "period = 11 ticks" calibration failure.  A write to `emr`
  with bit 0 set drops the *latched* EXT1 request without taking it
  (the kernel's `r=emr; emr=r|1; emr=r` pulse); the pin level is not
  affected.
- **FRMOVRN** — L5 bit 1 is a level view of the sticky `pdspFrameOvr`
  latch: it re-latches until the host clears the $21C bit itself
  (rtm-rom-host-side.md §2).

## What boot looks like (verified on the 7.1 AV image)

During the Enabler's RTM init (~65–90 M instructions into boot):
release → Apple's kernel boots out of the ROM's `'3210'` segments —
stage-1 handshake cell `$18`, then `data+$198` bits 0 **and** 1 (the
three-stage handshake) — StartFrames programs the 24 kHz / 240-frame
cadence and enables L5, the kernel calibrates its frame period against
EXT1 pulses (`data+$1D0` ≈ `$288B1` timer ticks), and the install
(`_DSPDispatch $9A`) admits the standard sound team and registers the
`sdev/'dsp '/appl` "Built-in" output component.  At rest the DSP stays
**alive** — kernel idle loop awaiting frame ticks, `emr == $8000` —
with the sound team resident.  (An earlier belief that the init probe
parks the DSP was an artifact of the broken pin model: with an 11-tick
measured period the install failed `$F5C8`/`$F5D0` and its *failure
cleanup* stopped the DSP.)  There is no `$81` timeout on a healthy
boot.  `suite-av` rows `av-dsp-boot` and `av-dsp-determinism` assert
all of this.

## Object node

`machine.dsp`: `state` (`reset`/`running`/`idle`/`crashed`), `pc`, `ps`,
`emr`, `pcw`, `sp` (r21), `evtp` (r22), `instr_count`; methods `step(n)`
and `disasm(addr, count)` (reads through the DSP's own bus view, so
on-chip RAM disassembles correctly).  No `$` aliases.

## Checkpointing

Glue POD + the core's plain-data blob (`DSP3210_CHECKPOINT_SIZE`,
including the 64 KB on-chip window) at the family slot after the PSC;
the burst event restores through the named type `"dsp"/"burst"`.  Hook
pointers are re-planted on restore.
