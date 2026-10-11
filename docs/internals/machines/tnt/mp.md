# The dual-processor card — Power Macintosh 9500/180MP

`src/machines/tnt/mp.c` models Apple's two-way processor card on the TNT
board: a second 604 that runs guest code as a **peer helper** of the main
CPU.  It is wired in by the `pm9500mp` profile (`pm9500.c`, board flag
`mp_cpus = 2`); on every other TNT board the file is inert (`st->cpu1` is
NULL, no event is registered, no object appears).  The core-side half — a
second `ppc_t` with its own translation caches — lives in
`src/core/cpu/ppc/` (docs/internals/core/cpu/ppc.md, "Peer cores").

Hardware reference: [pm9500mp.md](../../../reference/machines/tnt/pm9500mp.md)
(the card's register surface and lifecycle as established here),
[pm9600.md](../../../reference/machines/tnt/pm9600.md) §4.5 (the same card
on the 9600/200MP), [hammerhead.md](../../../reference/machines/tnt/hammerhead.md)
(the registers' home).  Guest side:
[beos.md](../../../reference/os/beos.md) §5.6–§5.9.

## What the card is, in model terms

Five behaviours and one lifecycle rule, all routed through `mp.c`:

| Surface | Where it is decoded | Model |
|---|---|---|
| Hammerhead `+$90` ArbConfig | `hammerhead.c` (`tnt_hh_init`) | `$02` (TwoCPU) at power-on on the MP board; accept-and-readback like every Hammerhead cell (the ROM read-modify-writes it) |
| Hammerhead `+$B0` WhoAmI | `tnt_hh_read` → `tnt_mp_whoami` | `$10` while CPU 0 runs, `$08` inside a CPU 1 burst — keyed on the issuing core, never stored |
| Hammerhead `+$C0` IntReg | `tnt_hh_write` → `tnt_mp_intreg` | a plain latch (power-on `$FF`); bit `$80` (SecInt) active low.  A write with the bit clear while CPU 1 is parked is the **start doorbell**; while CPU 1 runs, the bit is its external-interrupt level |
| `$F2800000` mailbox | `bandit.c` (Bandit 1's config-address latch) → `tnt_mp_mailbox` | snooped on every write; the start logic takes the bus value (`bswap` of the little-endian latch).  No config cycle is involved — Bandit only cycles on data-port access |
| `$F3019000` Ethernet PROM | `grand_central.c` (`OFF_EPROM`) → `tnt_mp_eprom_access` | an access **by CPU 1** pulses Grand Central line 30 (`TNT_INT_IPI`) — the primary's IPI.  CPU 0's own reads (station address) do not ring |

The lifecycle: CPU 1 is **parked** from reset.  The doorbell **calls** it at
the mailbox — `ppc_start_at`: the hard-reset register state (translation
off, MSR[IP]), PC = mailbox, LR = a parking spin (the first `b .` in the
top megabyte of the ROM, found once and kept in `mp.park_pc`).  An entry
that returns (`blr`) lands in the spin; the burst handler sees PC ==
`park_pc` and parks the core again (no event, zero cost).  An entry that
never returns is an operating system taking the processor over.

Why this shape — the evidence, from the three consumers that exist:

- **Open Firmware** reads ArbConfig, and on TwoCPU writes the mailbox with
  `$00408B38` and rings (`$00` to IntReg).  That address holds
  `mfspr r0,pvr; stw r0,12(r1); blr`: with reset registers the secondary
  stores its PVR at physical `$0C` and returns.  So OF uses the card to
  read the second processor's version, and expects the core to come back
  to rest — which is the call/park contract, and the only reading under
  which `blr` into a zero LR is not a crash.  Firmware-level MP code was
  not expected (the ROM's 68k and NanoKernel halves have none); it lives in
  the Forth `/chosen` construction.
- **BeOS 5.0.3** (`kernel_mac`, `start_other_cpus`) writes `$000568AC` to
  the mailbox and rings with `atomic_and(IntReg, $7FFFFFFF)` — *over the
  `$00` Open Firmware left there*.  The kick is therefore the write, not a
  1→0 edge.  CPU 1's entry code acknowledges with `atomic_or(IntReg,
  $80000000)` while CPU 0 polls for the bit.
- **Linux** (`smp_psurge_kick_cpu`) writes the entry, asserts, deasserts,
  and afterwards rewrites the mailbox to `$100` "so if we get another intr
  we won't try to startup again" — consistent with later interrupts
  reaching a running core.

The Ethernet-PROM direction rule is forced by BeOS too: its bootstrap reads
the six station-address bytes from `$F3019000+$10n` to compare with OF's
`local-mac-address`, and its pre-kernel interrupt poll treats a pending line
30 as an IPI it has no handler to clear — a primary-side ring would
livelock it.  Every sender of the primary's IPI is the secondary.

## Execution: bursts on the one timeline

CPU 0 owns emulated time, unchanged.  Once CPU 1 runs, a periodic scheduler
event (`"cpu1"/"burst"`, every `MP_QUANTUM_CYCLES` = 4096 cycles) gives it
as many instructions as CPU 0 retired since the previous burst
(`scheduler_instr_count` delta, capped at 64 K) — same clock, same CPI
model, so the two advance at one rate and CPU 1 trails by at most a quantum
(~23 µs at 180 MHz).  Around each burst:

- **The fast path swaps** (`ppc_mmu_activate`): CPU 1's fetch window, user
  SoA arrays and active pair go live; CPU 0's are parked.  The supervisor
  arrays (the physical identity view) are shared — physical memory is one.
- **The sprint channel closes**: `g_io_cpi_x256 = 0` and
  `g_sprint_burndown_ptr = NULL`, so I/O penalties are not charged to a
  CPU 0 sprint; `g_bus_error_instr_ptr` is saved and restored around
  `ppc_run`.
- **Reservations drop**: both cores' `lwarx` reservations are cleared at
  every switch (`ppc_clear_reservation`).  A core that has not run since
  the other one did cannot hold a valid reservation under the conservative
  "any peer store clears" rule, and the architecture allows a reservation
  to be lost at any time; a `stwcx.` that straddles a switch simply retries.
- **WhoAmI** answers `$08` (`mp.in_cpu1`).

Coherence beyond that is free: bursts serialize, which gives a sequentially
consistent memory history (stronger than the 60x bus's), so `sync`,
`eieio` and `isync` need nothing.  `tlbie` broadcasts in the core
(`ppc_mmu_tlbie` invalidates every context's caches — the 604 snoops bus
`tlbie`s).

Time: each core keeps its own timebase/decrementer derived from the shared
cycle counter (`ppc_bind_time_named`, CPU 1's DEC event is `"cpu1"/"dec"`),
so the two timebases advance in lockstep and differ only by what software
writes.  BeOS zeroes both at its synchronisation point; the card's
**timebase-freeze** across Linux's kick window is not modelled (see Known
debts).  Within a burst the timebase does not advance (time moves between
bursts), so a CPU 1 delay loop burns its burst and finishes in a later one.

Idle cost: none while parked.  Under BeOS both cores spin in the idle loop
(`$EAB0`), so a running MP guest costs ~2× interpretation — accepted, and
deliberately not mitigated by idle-loop detection.

## Object model

- `machine.cpu1` — the second core's standard PPC node (registers, `mmu`,
  `fpu`, `frame`, `disasm`); `instr_count` reads the card's count, not the
  scheduler's.  No `$` aliases (they stay CPU 0's).
- `machine.mp` — `running`, `calls` (doorbell calls since reset),
  `entry` (the vector the last call used), `mailbox` (the latch as it is
  now — Bandit 1's config address, which later PCI accesses overwrite),
  `park_pc`, `bursts`, `cpu1_instr`.  A Mac OS boot of the MP board leaves
  `calls == 1, running == false, entry == $00408B38` (Open Firmware's PVR
  probe); BeOS leaves `calls == 2, running == true, entry == $000568AC`.
- Log category `mp`: level 1 = calls and parks, level 3 = every SecInt
  change and PriInt ring.

## Checkpointing

The `"cpu1"` machine part (after `"cpu"`, `"scheduler"`) holds `tnt_mp_t`
(POD, `in_cpu1` always 0 at a save) followed by CPU 1's `ppc_t` prefix.
Its two event types (`"cpu1"/"burst"`, `"cpu1"/"dec"`) are registered at
construction, before `scheduler_start`, so a restore of a running MP guest
re-arms both.  CPU 1's translation caches are host-pointer state and refill
lazily, like CPU 0's.  A running BeOS SMP desktop round-trips.

## Testing

- `tests/integration/tnt-beos` — BeOS 5.0.3 on the uniprocessor 9500 and on
  the 9500/180MP: Mac OS 7.6 boots, `BeOS_Launcher` (in Startup Items)
  hands over, BeOS reaches the Tracker desktop; on the MP row the card's
  truths (`calls`, `mailbox`, `running`, CPU 1 retiring instructions) are
  asserted before the golden.
- `suite-tnt` row `pm9500mp-76-hd` — Mac OS 7.6 on the MP board: OF's PVR
  probe calls and parks CPU 1, and the boot is otherwise the 9500's.
- `tests/unit/suites/ppc` — `mfspr` of the TB numbers.

## Known debts

- **604e identity.**  The card carried 604e's; the profile reports the 604
  PVR because Open Firmware in both 1995 TNT ROMs stops before building the
  device tree on PVR `$0009xxxx`.  The 1997 ROM (`960E4BE9`) knows the
  604e; pairing it with the profile is the follow-up.
- **Timebase freeze.**  The card's TB-freeze-across-the-kick (Linux's
  two-phase sync) is not modelled; BeOS does not need it.
- **Debugger.**  Breakpoints, PC logpoints and single-step act on CPU 0
  only; memory logpoints fire on CPU 1's accesses, but a logpoint installed
  while CPU 1 is parked drops CPU 1's private user fills wholesale rather
  than per page.
- **Accelerated mode.**  CPU 1's budget is CPU 0's retired count, so the
  governor scales both together (the intended policy).
