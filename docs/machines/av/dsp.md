# AV DSP3210 integration (`src/machines/av/dsp.c`)

The AT&T DSP3210 — the AV family's floating-point DSP — as a live
auxiliary core.  The generic core lives in `src/core/cpu/dsp3210/`
(adapted from the validated dossier core, see the header of
`dsp3210.h`); this file binds it to the board.  Contracts:
`local/gs-docs/840av_660av/docs/dsp3210.md` §8 and the gap-closure
findings in `local/gs-docs/dsp3210-plaintalk/`.

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
  decodes it internally).  Accesses at `$40000000`+ fault → DSP
  bus-error vector 1 → Apple's own `'xbus'` crash dump.
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
  timer (vector 9) is the kernel's steady-state heartbeat.
- **FRMOVRN** — L5 bit 1 is a level view of the sticky `pdspFrameOvr`
  latch: it re-latches until the host clears the $21C bit itself
  (rtm-rom-host-side.md §2).

## What boot looks like (verified on the 7.1 AV image)

During the Enabler's RTM init (~65–90 M instructions into boot):
release → Apple's kernel boots out of the ROM's `'3210'` segments —
stage-1 handshake cell `$18`, then `data+$198` bits 0 **and** 1 (the
three-stage handshake) — StartFrames programs the 24 kHz / 240-frame
cadence and enables L5, the kernel calibrates against EXT1 pulses and
parks on its timer… and the init probe then deliberately **stops** the
DSP (`ShutDownProcessor`: clear `pFrmIntEn`, `$83`, disable L5, clear
the sticky latch) until a sound client needs it.  There is no `$81`
timeout on a healthy boot.  `suite-av` rows `av-dsp-boot` and
`av-dsp-determinism` assert all of this.

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
