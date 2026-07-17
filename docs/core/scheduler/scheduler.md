# Scheduler: Cycle and Time Bookkeeping

This document describes the internal timing model of the Granny Smith scheduler in detail:
how emulated time advances, how CPU execution is organized into sprints, how events are
queued and fired, and how the various counters stay consistent with each other.

The source lives in [src/core/scheduler/scheduler.c](../src/core/scheduler/scheduler.c) and
[src/core/scheduler/scheduler.h](../src/core/scheduler/scheduler.h).

---

## 1. Introduction

The scheduler is the heartbeat of the emulator. It has four jobs:

1. **Advance emulated CPU time** by running the CPU for bursts of instructions (we call
   these bursts *sprints*).
2. **Fire timed events** (VBLs, VIA T1/T2 underflows, SCC/SCSI completions, audio DMA
   refills, etc.) at exactly the cycle count they were scheduled for.
3. **Keep bookkeeping consistent** — at any moment, code anywhere in the emulator can ask
   "what's the current cycle count?" or "how many instructions have run?" and get a
   correct answer, even in the middle of executing an instruction.
4. **Pace execution to the platform's needs.** The WASM target advances guest time at
   the host's render rhythm so the emulated Mac feels real (~60 VBL/s of host time);
   the headless target ignores host time entirely and runs as fast as possible with
   VBLs scheduled as ordinary cycle-driven events. See §10.

### 1.1 Mental model in one picture

```
Emulated CPU time (cycles) -->
    |           |                 |         |            |
    ev(A)       ev(B)             ev(C)     ev(D)       ev(E)
    │           │                 │         │            │
    ├── sprint ─┤                 │         │            │
                ├──── sprint ─────┤         │            │
                                  ├─sprint──┤            │
                                            ├─── sprint ─┤
```

Time flows left to right. Events (`ev(A)`..`ev(E)`) sit at fixed cycle timestamps. Each
sprint runs CPU instructions up to — but not past — the next event, then the event fires,
then a new sprint is planned. **Sprints exist to serve events, not the other way around.**

### 1.2 Common misconceptions (read this first)

These are myths that repeatedly show up in discussions of the scheduler. Every one is
**false**:

- **"Sprints are time quanta, so events fire with sprint-length jitter."**
  No. The sprint length for every iteration is computed as
  `min(remaining_budget, cycles_to_next_event)`. If an event is 37 cycles away, the
  sprint executes roughly 37/CPI instructions and stops *at* the event. Events fire at
  the exact cycle they were scheduled for, up to a bounded "instruction atomicity
  overshoot" of at most `CPI - 1` cycles (see §8).

- **"Interrupts have to wait until the current sprint naturally ends before the CPU
  sees them."**
  No. When a device changes the IRQ line it calls `cpu_reschedule()`, which sets
  `sprint_burndown = 0` (see §4.3). The CPU decoder's inner loop
  (`while (*instructions > 0)`) exits at the next instruction boundary, the sprint
  finalizes, and the next iteration of the sprint loop calls `cpu_run_sprint` again —
  whose prologue runs `cpu_check_interrupt` and takes the exception
  ([cpu_68000.c:66](../src/core/cpu/cpu_68000.c#L66)). End-to-end IRQ latency from line
  assertion to exception vector entry is therefore at most one instruction plus the
  sprint-boundary overhead. Sprints are *not* uninterruptible time quanta — any IRQ
  line change cuts the current sprint short.

  What is true: the CPU never takes an interrupt *mid-instruction*. Instruction
  atomicity still holds (§8). The 68000 itself samples IPL lines at instruction
  boundaries, so this matches real hardware.

- **"`cpu_cycles` is continuously updated as the CPU executes."**
  No. `s->cpu_cycles` is the **authoritative cycle counter at the last completed sprint
  boundary.** During a sprint it is deliberately stale. The "live" current time is
  derived on demand by `current_cpu_cycles(s)`, which adds in-sprint progress on top of
  `s->cpu_cycles`.

- **"A sprint always executes the full number of instructions it was set up with."**
  Usually yes, but an I/O access with bus wait states may deduct phantom instructions
  from `sprint_burndown` mid-flight, ending the sprint early. This is how the emulator
  keeps cycle accounting honest across slow I/O (see §6.2).

- **"Switching scheduler mode (paced/turbo) changes what the guest executes."**
  No. Those two modes are *pacing only* — they decide how many frame-units a host tick
  batches, never how many instructions a frame-unit contains. CPI is a per-machine
  constant independent of the mode, so `cpu_cycles`, the instruction count, and the
  guest's execution sequence are identical in both modes (and on both targets) for a
  given frame-unit count — see §9. The **`accelerated`** mode is the deliberate
  exception: it lowers the *effective* CPI so a frame-unit retires more instructions
  (§2.2), which is guest-visible by design — and why it is excluded from budget-pinned
  tests (§10.4).

---

## 2. Time base: cycles are ground truth

All timing in the emulator is ultimately measured in **CPU cycles** of the emulated
68000/68030. The clock frequency is per-machine (Mac Plus: 7,833,600 Hz; SE/30:
15,667,200 Hz) and stored in `s->frequency`. Nanoseconds and microseconds are computed
on demand from cycles:

```c
ns = cycles * NS_PER_SEC / s->frequency;     // cycles -> ns
cycles = ns * s->frequency / NS_PER_SEC;     // ns -> cycles
```

Cycles are the source of truth. Instruction counts are derived from cycles using the
per-machine cycles-per-instruction (CPI) constant, and they are *not* cycle-perfect —
the emulator models "average" CPI, not per-opcode cycle timings.

### 2.1 Cycles-per-instruction (CPI)

CPI is **one constant per machine**, set at init via `scheduler_set_cpi(s, cpi)` and
independent of the pacing mode:

| Machine family | CPI | Notes                                                        |
|----------------|-----|--------------------------------------------------------------|
| Plus (68000)   | 12  | Authentic average for the 7.8336 MHz 68000                   |
| 030 machines (IIcx/SE30/IIx/IIci/IIsi/IIfx) | 4 | 4-clock bus cycle, 1-wait-state RAM |
| Lisa / Mac XL  | 4   | Historical effective value (every Lisa budget derives from it) |

The default (no `scheduler_set_cpi` call) is 12
([scheduler.c](../src/core/scheduler/scheduler.c)). The `scheduler.cpi` attribute is
writable as a **debug override** (1..255); changing it mid-run alters the guest
timeline from that point on, so it is a tuning tool only.

**Key consequence:** because CPI is constant, the guest's execution sequence is a pure
function of the frame-unit count — in every pacing mode and on both targets. This is
the **one guest timeline** property (§9, §10.4).

> **History.** Before the two-mode change
> (`local/gs-docs/completed/proposal-scheduler-two-modes.md`), CPI depended on the
> scheduler mode (`cpi_hw` = 12 in `hw_accuracy`, `cpi_fast` = 4 elsewhere), which made
> the cycles↔instructions relationship piecewise and mode switches guest-visible. On
> the Plus this also meant the default mode emulated a ~3× overclocked 68000; the
> authentic CPI 12 is now the Plus constant.

### 2.2 Effective CPI and the accelerated mode

The `accelerated` mode (proposal-scheduler-accelerated-mode.md, stage 1: fixed
multiplier) re-introduces a *faster-than-authentic* CPU as an **explicit, opt-in
mode** — the "CPU accelerator card" model: the machine keeps its real-time timebase
(VBL, VIA φ2, sound, SCC) while the CPU retires more instructions per frame-unit.

The mechanism is a **fractional effective CPI** (`cpi_eff_x256`, x256 fixed point),
derived as `authentic_cpi / scheduler.speed` and used by all sprint sizing and cycle
accounting. The authentic CPI stays the anchor and the floor — the mode only ever
tunes *below* it (faster), never above. Why lower CPI instead of raising `frequency`:
every cycle-derived peripheral clock (VIA φ2 = `cpu_cycles / freq_factor`, the Plus's
352-cycle sound scan, the SCC cycle fallback) divides the **raw cycle count** by a
fixed divisor. Holding the cycle rate constant and tuning instructions-per-cycle
keeps all of them real-time for free; raising the frequency would require rescaling
every divisor.

Sprint cycle accounting carries the sub-cycle remainder (`cycle_frac_x256`) in
scheduler state, so cycles advanced stay exact integers and nothing is dropped:
`current_cpu_cycles()` mid-sprint, the end-of-sprint bookkeeping, and the E-sync
penalty's reconstructed "now" all use the same `(slots × cpi_eff_x256 + frac) >> 8`
formula. With the authentic integer CPI (paced/turbo) the remainder is identically
zero and every code path is bit-identical to the pre-fractional arithmetic — pinned
budgets do not move.

`scheduler.speed` picks the multiplier: **0 = auto** (the adaptive governor, the
default) or 1.0 .. 8.0 to pin a fixed multiplier. The *setting* (and the
`scheduler.max_speed` cap) persist in the checkpoint prefix while the derived
`cpi_eff_x256`, the remainder, and all governor state are transient and
re-derived/cleared on restore and on every mode/CPI/speed change.

### 2.3 The adaptive governor (speed = auto)

With `scheduler.speed = 0`, a closed-loop governor (stage 2 of the proposal, §4)
picks the highest speed the host can sustain *without missing the real-time
deadline* — overrunning it stalls the paced accumulator, which surfaces as audio
underruns and stutter. Per paced main-loop tick that executed frame-units, it:

1. **Measures** utilization `u = host seconds to emulate one frame-unit /
   MAC_VBL_PERIOD` (the same measurement the pacing EWMA already makes), smoothed
   with a fast-up/slow-down EWMA so pressure registers quickly.
2. **AIMD on a quantized ladder** (1×, 1.5×, 2×, 3×, 4×, 6×, 8×): one rung **down**
   immediately when the smoothed utilization crosses 0.90 or the (optional) audio
   ring drains below half its target depth; one rung **up** only after ≥2 s of
   residence at the current rung, outside a 1 s post-back-off holdoff, and only if
   the utilization *projected at the next rung* stays under the 0.80 target.
3. **Bounds**: floor = the authentic CPI (rung 0 — accelerated never runs slower
   than real hardware; an overloaded host degrades to exactly `paced` behavior);
   ceiling = `scheduler.max_speed` (default 8×, persisted).

The dwell/holdoff slew limit and the coarse quantization are **correctness
requirements**, not tuning niceties (proposal §4.4): the guest calibrates
instruction-counted delays (`TimeDBRA` etc.) against the VIA at boot, so the
machine must dwell at stable speeds rather than glide. The utilization EWMA is
rescaled on every step (utilization is proportional to instructions per frame),
keeping the estimator meaningful across steps.

**Audio feedback** (§11.3): `platform_audio_ring_fill()` reports the host ring's
fill fraction against its target depth, or `< 0` where the signal doesn't exist —
headless, or audio idle. On wasm the worklet posts periodic fill reports which are
cached and piggybacked back to the emulator thread on the (already main-thread-
proxied) push call — no extra round trips. The governor treats the signal as
strictly optional.

**Headless**: the governor's input is the paced main loop's host-timing signal,
which the budget-driven headless path never produces — so accelerated-auto stays
at the authentic floor there. Headless acceleration uses a pinned
`scheduler.speed`, which is also the §4.4-safe configuration (a constant speed is
self-consistent with the guest's boot-time calibration).

---

## 3. Core data structures

### 3.1 The scheduler struct

The entire scheduler state is encapsulated in one opaque struct
([scheduler.c:68-100](../src/core/scheduler/scheduler.c#L68)). The fields that matter
for timing:

```c
struct scheduler {
    enum schedule_mode mode;         // paced | unthrottled | accelerated
    uint32_t cpi;                    // Per-machine authentic CPI constant
    uint32_t speed_x256;             // Accelerated-mode multiplier setting (persisted)
    uint32_t frequency;              // CPU clock in Hz

    uint32_t cpi_eff_x256;           // Effective CPI, x256 (derived; not checkpointed)
    uint32_t cycle_frac_x256;        // Sub-cycle sprint remainder (transient)

    uint64_t cpu_cycles;             // Authoritative "now" at last sprint boundary
    uint64_t total_instructions;     // Instructions retired through last sprint boundary
    uint32_t sprint_total;           // Instructions planned for the current sprint
    uint32_t sprint_burndown;        // Instructions still to execute in this sprint

    event_t *cpu_events;             // Priority queue, sorted by timestamp

    // ...host-timing, checkpointing, and event-type registry elided...
};
```

The four bookkeeping counters — `cpu_cycles`, `total_instructions`, `sprint_total`,
`sprint_burndown` — together describe the exact state of emulated time. §4 explains how
they work in detail.

### 3.2 Events

An event is a callback scheduled to fire at an absolute cycle timestamp
([scheduler.c:43-49](../src/core/scheduler/scheduler.c#L43)):

```c
struct event {
    event_t *next;                   // Linked-list pointer (singly linked)
    uint64_t timestamp;              // Absolute cycle count when event should fire
    event_callback_t callback;       // void (*)(void *source, uint64_t data)
    void *source;                    // Opaque owner pointer (e.g. the VIA)
    uint64_t data;                   // Opaque payload passed to callback
};
```

The event queue `s->cpu_events` is a singly-linked list kept in **non-decreasing
timestamp order**. The head is the next event to fire.

---

## 4. Cycle and instruction bookkeeping — in detail

This is the most misunderstood part of the scheduler. Read it carefully.

### 4.1 The four counters

| Field                 | Meaning                                                              | Updated when                               |
|-----------------------|----------------------------------------------------------------------|--------------------------------------------|
| `cpu_cycles`          | Authoritative "now," in cycles, **as of the last sprint boundary**.  | End of each sprint iteration.              |
| `total_instructions`  | Retired instruction count **as of the last sprint boundary.**        | End of each sprint iteration.              |
| `sprint_total`        | Instructions the current sprint was *planned* to execute.            | Set at sprint setup; zeroed at sprint end. |
| `sprint_burndown`     | Instructions still remaining in the current sprint.                  | Decremented by the CPU decoder per instr. |

Between sprints, `sprint_total == sprint_burndown == 0`. During a sprint,
`0 ≤ sprint_burndown ≤ sprint_total`.

### 4.2 Deriving "live" now from the counters

`s->cpu_cycles` is deliberately stale during a sprint. When code elsewhere in the
emulator asks for the current cycle or instruction count, the scheduler computes the
answer on the fly:

```c
// scheduler.c:180
static uint64_t current_cpu_cycles(struct scheduler *s) {
    uint64_t in_sprint = s->sprint_total - s->sprint_burndown; // instr executed so far in this sprint
    return s->cpu_cycles + in_sprint * avg_cycles_per_instr(s);
}
```

```c
// scheduler.c:826
uint64_t cpu_instr_count(void) {
    return s->total_instructions + s->sprint_total - s->sprint_burndown;
}
```

Both formulas use the same quantity — instructions consumed in the current sprint —
but scale it two different ways (by CPI for cycles, untouched for instructions).

**Why this scheme?** It lets the CPU decoder hot-loop decrement a single `uint32_t`
(`sprint_burndown`) per instruction without having to touch `cpu_cycles`,
`total_instructions`, or the event queue. Reconciliation happens only when it actually
matters (sprint end, mid-sprint event scheduling, IRQ line change).

### 4.3 Reconciling the sprint counters

When something mid-sprint needs a stable view of the counters (scheduling a new event,
reacting to an IRQ line change), the scheduler **reconciles** without actually ending
the sprint:

```c
// scheduler.c:208
static void reconcile_sprint(struct scheduler *s) {
    uint32_t executed = s->sprint_total - s->sprint_burndown;
    s->sprint_total = executed;      // shrink the plan to what's already done
    s->sprint_burndown = 0;          // no more pending work in this "plan"
}
```

After reconciliation:

- `cpu_instr_count()` still returns the same value (it depends on `sprint_total -
  sprint_burndown`, which is unchanged).
- `current_cpu_cycles(s)` still returns the same value (same reason).
- Any new event scheduled gets a timestamp computed from a now-stable baseline.
- The CPU decoder is still running; it will see `sprint_burndown == 0` and exit its
  inner loop at the next instruction boundary.

Critically, **`reconcile_sprint()` does not update `s->cpu_cycles` or
`s->total_instructions`**. Those get updated only in the sprint-finalization code at
[scheduler.c:947-960](../src/core/scheduler/scheduler.c#L947). This is what makes it
safe to call from deep inside a memory write.

### 4.4 Worked example

Assume a machine with CPI=4 (any pacing mode — CPI doesn't depend on it). The
scheduler finished its last sprint at cycle 1,000 having retired 250 instructions.
State:

```
cpu_cycles=1000  total_instructions=250  sprint_total=0  sprint_burndown=0
```

Next event is at cycle 1,040. We plan a sprint of `(1040 - 1000) / 4 = 10` instructions:

```
cpu_cycles=1000  total_instructions=250  sprint_total=10  sprint_burndown=10
```

The decoder runs 3 instructions:

```
cpu_cycles=1000  total_instructions=250  sprint_total=10  sprint_burndown=7
                                              ^^ still stale
current_cpu_cycles(s) = 1000 + (10 - 7) * 4 = 1012
cpu_instr_count()     = 250  + (10 - 7)     = 253
```

A VIA write at this point schedules a new event "in 100 cycles." Scheduler calls
`reconcile_sprint()`:

```
cpu_cycles=1000  total_instructions=250  sprint_total=3  sprint_burndown=0
current_cpu_cycles(s) = 1000 + 3 * 4 = 1012   (unchanged)
cpu_instr_count()     = 250 + 3      = 253    (unchanged)
```

The new event is inserted at timestamp `1012 + 100 = 1112`. The decoder's next check of
`sprint_burndown` sees 0 and exits the inner loop. The sprint finalizer then runs:

```
executed = sprint_total = 3
total_instructions += 3          -> 253
cpu_cycles += 3 * 4              -> 1012
sprint_total = 0
```

Finally `process_event_queue` fires any events with `timestamp <= 1012` — the
1,040-cycle event is still 28 cycles away, so no event fires yet, and a new sprint is
planned.

---

## 5. Scheduling an event

```c
event_t *scheduler_new_cpu_event(scheduler, callback, source, data, cycles, ns);
```

Exactly one of `cycles` or `ns` must be non-zero
([scheduler.c:754-755](../src/core/scheduler/scheduler.c#L754)); the other is derived
from the machine frequency. The pipeline
([scheduler.c:749-764](../src/core/scheduler/scheduler.c#L749)):

1. **Invariant check** (`CHECK_INVARIANTS(s)`) and queue integrity check
   (`validate_cpu_events`).
2. **`reconcile_sprint(s)`** — stabilize the counters so `current_cpu_cycles(s)` is
   accurate for the next step.
3. **`add_event_internal`**:
   - `now = current_cpu_cycles(s)`
   - `event->timestamp = now + cycles`
   - `insert_event_queue` — walk the list, insert at the first position where the next
     node has a strictly greater timestamp. New events tie-break *after* existing events
     with the same timestamp (see the loop at [scheduler.c:262](../src/core/scheduler/scheduler.c#L262)).
4. **Invariant check** again.

Because `now` is computed *after* `reconcile_sprint`, the new event is always placed
relative to an accurate baseline — not to the stale `s->cpu_cycles`.

### 5.1 Event removal

- `remove_event(s, callback, source)` — remove every event matching `callback` and
  `source` (pass `source = NULL` to match any source).
- `remove_event_by_data(s, callback, source, data)` — as above but also match `data`.
- `has_event(s, callback)` — check whether any event with that callback is scheduled.

Both remove variants walk the full list; removing an event is `O(n)`.

---

## 6. Running the CPU: the sprint loop

The main execution path is
[scheduler_run_instructions](../src/core/scheduler/scheduler.c#L891). At a high level:

```c
while (remaining_cycles > 0) {
    // 1. Decide how far to run this sprint
    cycles_to_execute = remaining_cycles;
    if (cpu_events != NULL) {
        cycles_to_event = cpu_events->timestamp - cpu_cycles;   // never negative (invariant)
        cycles_to_execute = MIN(cycles_to_event, remaining_cycles);
    }
    instr_to_exec = cycles_to_instructions(s, cycles_to_execute);
    if (cycles_to_execute > 0 && instr_to_exec == 0) instr_to_exec = 1;
    if (debugger_active) instr_to_exec = 1;                      // single-step in debugger

    // 2. Execute the sprint
    sprint_total = sprint_burndown = instr_to_exec;
    g_sprint_burndown_ptr = &sprint_burndown;                    // expose to I/O penalty mechanism
    g_io_cpi_x256 = cpi_eff_x256;                                // effective CPI (== cpi<<8 unless accelerated)
    g_io_phantom_instructions = 0;
    cpu_run_sprint(cpu, &sprint_burndown);                       // CPU runs until burndown hits 0
    g_sprint_burndown_ptr = NULL;

    // 3. Finalize: fold the sprint's work into the authoritative counters
    executed_slots = sprint_total;                               // may have been shrunk by reconcile
    phantom = g_io_phantom_instructions;
    sprint_total = 0;
    advance_x256 = executed_slots * cpi_eff_x256 + cycle_frac_x256;
    executed_cycles = advance_x256 >> 8;                         // whole cycles advance…
    cycle_frac_x256 = advance_x256 & 0xFF;                       // …the fraction carries (0 at integer CPI)

    total_instructions += (executed_slots - phantom);            // only real instructions count
    cpu_cycles += executed_cycles;                               // but all cycles count
    remaining_cycles -= executed_cycles;                         // (saturating at fractional CPIs)

    // 4. Fire events due at or before the new cpu_cycles
    process_event_queue(&cpu_events, cpu_cycles);
}
```

### 6.1 How sprint length is chosen so events fire on time

Look at step 1 carefully: `cycles_to_execute` is **clamped** to the gap between the
current `cpu_cycles` and the timestamp of the head event. The sprint will not execute
past the next event — unless instruction atomicity forces it (§8).

This is the mechanism that guarantees precise event timing. Sprints are **shaped around
events**, not the other way around. A long "max-speed" burst of instructions still gets
chopped into short sprints if events are dense on the queue.

### 6.2 I/O penalties shorten sprints mid-flight

Some memory accesses (slow I/O, bus stalls on SE/30 PDS) cost more than CPI cycles.
Rather than lie about this, the emulator converts the surplus cycles into **phantom
instructions** that are deducted from `sprint_burndown`:

```c
// memory.h — memory_io_penalty
g_io_penalty_remainder += extra_cycles << 8;  // whole cycles onto the x256 grid
burn = g_io_penalty_remainder / g_io_cpi_x256;
if (burn > 0) {
    g_io_penalty_remainder -= burn * g_io_cpi_x256;
    g_io_phantom_instructions += burn;
    *g_sprint_burndown_ptr -= burn;           // ends the sprint sooner
}
```

Effect: an I/O-heavy sprint finishes after fewer real instructions than planned, but
the cycle count still advances correctly because `cpu_cycles` is increased by
`sprint_total * CPI` — including the phantom instruction slots. The `phantom` count
is subtracted from `total_instructions` in step 3 above so the *instruction* counter
only reflects real work.

The `g_io_penalty_remainder` fraction deliberately **persists across sprints** so
sub-CPI penalties accumulate correctly over time ([scheduler.c:939](../src/core/scheduler/scheduler.c#L939)).

### 6.3 IRQs cut the current sprint short

IRQ lines are raised by devices at arbitrary points (VIA timer expiry, SCC RX ready,
etc.), usually from inside an event callback. The code path is:

```
device raises IRQ -> machine_update_ipl() -> cpu_set_ipl(cpu, level) -> cpu_reschedule()
```

`cpu_reschedule()` is just `reconcile_sprint()` on the global scheduler
([scheduler.c:835](../src/core/scheduler/scheduler.c#L835)). It sets
`sprint_burndown = 0` while leaving all derived quantities (`current_cpu_cycles`,
`cpu_instr_count`) intact. That has two effects:

1. **The sprint ends at the next instruction boundary.** The CPU decoder's inner loop
   `while (*instructions > 0)` in [cpu_68000.c:67](../src/core/cpu/cpu_68000.c#L67)
   sees 0 and returns.
2. **Any events that the IRQ handler itself scheduled get accurate "now" timestamps**,
   because the reconcile happened before `add_event_internal` reads
   `current_cpu_cycles(s)`.

After the sprint returns, the sprint loop finalizes counters, drains due events via
`process_event_queue`, and iterates. The next call to `cpu_run_sprint` runs
`cpu_check_interrupt` in its prologue ([cpu_68000.c:66](../src/core/cpu/cpu_68000.c#L66))
and enters the exception handler.

End-to-end latency from `cpu_reschedule()` to exception-vector entry is therefore at
most **one instruction** (the one in flight when burndown was zeroed) plus the
sprint-loop-iteration overhead. This matches real 68000 behavior, which also samples
IPL at instruction boundaries.

The same mechanism is what `scheduler_stop()` uses
([scheduler.c:843](../src/core/scheduler/scheduler.c#L843)): it sets `running = false`
and calls `reconcile_sprint` to cut the sprint short at the next boundary so the outer
`while (remaining_cycles > 0)` loop can exit promptly.

---

## 7. Event firing and callback semantics

After sprint finalization, `process_event_queue(&queue, cpu_cycles)` drains every event
with `timestamp <= cpu_cycles`:

```c
// scheduler.c:302
while (*queue != NULL && (*queue)->timestamp <= current_time) {
    event_t *e = *queue;
    *queue = e->next;
    (e->callback)(e->source, e->data);     // callback may schedule more events
    free(e);
}
```

A few important properties:

- **Multiple events may fire in a single call** if their timestamps are all `<=
  cpu_cycles`. This happens naturally when two events are scheduled at the same tick,
  or when the sprint overshot both (§8).
- **Callbacks may schedule new events.** Those new events are inserted into the queue
  via `scheduler_new_cpu_event`, which goes through the full reconcile → insert path.
  Because the new event gets `timestamp = current_cpu_cycles(s) + cycles`, it is always
  in the future relative to the current sprint baseline.
- **Callbacks run with `sprint_total = 0`** (the sprint has been fully reconciled at
  this point). So `cpu_instr_count()` and `scheduler_cpu_cycles(s)` report the exact
  end-of-sprint value.

---

## 8. Instruction atomicity and the overshoot bound

The CPU decoder cannot stop mid-instruction: once it has started decoding, it runs the
instruction to completion. This is the *only* source of timing imprecision in the
scheduler.

### 8.1 The overshoot scenario

Consider an event scheduled at timestamp `T` while `cpu_cycles = T - N`:

- If `N >= CPI`: `cycles_to_instructions(N)` returns `floor(N / CPI)`, and the sprint
  consumes at most `N` cycles, stopping at or before `T`.
- If `0 < N < CPI`: we still need to execute at least one instruction to make progress,
  so `cycles_to_instructions` rounds up to 1 ([scheduler.c:199](../src/core/scheduler/scheduler.c#L199)).
  The sprint consumes `CPI` cycles, overshooting the event by `CPI - N` cycles.

Worked example with `CPI = 12` (hw-accuracy mode):
- Event at `T + 11`, `cpu_cycles = T`.
- `cycles_to_instructions(11)` = 1 (the "at least 1" clause).
- Sprint runs 1 instruction = 12 cycles. `cpu_cycles` becomes `T + 12`.
- Event at `T + 11` is now 1 cycle "in the past" — fires on this iteration's
  `process_event_queue` call.

**Maximum overshoot is `CPI - 1` cycles**, which at 12 CPI is ~1.5 µs of emulated time.

### 8.2 Relaxed invariant

The strict invariant "every queued event has `timestamp >= cpu_cycles`" is therefore
relaxed to:

```
every queued event satisfies: timestamp + CPI >= cpu_cycles
```

Both `validate_cpu_events` and `scheduler_check_invariants` enforce the relaxed form
([scheduler.c:218-235](../src/core/scheduler/scheduler.c#L218),
[scheduler.c:144-149](../src/core/scheduler/scheduler.c#L144)). This allows the brief
window during a sprint iteration where the head event is slightly in the past but has
not yet been processed.

### 8.3 Mitigations when overshoot matters

If a device needs tighter-than-CPI timing (e.g., an SCC shift register), the common
patterns are:

- Schedule the event farther in advance so `N >> CPI`.
- Re-read the current time inside the callback via `scheduler_cpu_cycles(s)` and
  compensate (most VIA/SCC device models already do this for edge timings).

---

## 9. Mode switching and the CPI invariant

Between `paced` and `unthrottled`, `scheduler_set_mode` touches **only pacing state**
(it resets the wall-clock estimators, §10.3); CPI never changes with the mode. As long
as the machine never enters `accelerated` and the `scheduler.cpi` debug override is
untouched, the linear relationship

```
cpu_instr_count() * CPI == cpu_cycles
```

holds globally, and switching modes mid-run creates no discontinuity of any kind in
the guest timeline — `cpu_cycles` stays monotonic and future instructions keep
advancing it at the same rate.

Entering or leaving `accelerated` re-derives the effective CPI and clears the
fractional remainder symmetrically, so no accelerated-mode speed leaks into the other
modes; but time spent accelerated advances `cpu_cycles` at fewer cycles per
instruction, making the relationship above piecewise from then on (like a
`scheduler.cpi` override would).

The authoritative quantity is still `cpu_cycles`. If you need elapsed emulated time,
use cycles (or `scheduler_time_ns`); instruction counts are a derived, display-level
view.

**Checkpoint restore:** the restore path rebuilds `total_instructions` as
`cpu_cycles / cpi` ([scheduler.c](../src/core/scheduler/scheduler.c)). With a constant
CPI this reconstruction is exact for a timeline that never ran accelerated
(accelerated time makes it an underestimate — acceptable for a display-only counter).

---

## 10. VBL injection and per-target pacing

Both targets run the **same execution model**, built from one atomic step — the *VBL
frame-unit*. They differ only in **pacing**: how fast the run loop issues frame-units.

| Target   | Goal                                              | Pacing                                | Run loop                          |
|----------|---------------------------------------------------|---------------------------------------|-----------------------------------|
| Headless | Reproducibility — same budget ⇒ identical output  | As fast as the host allows (unthrottled) | pump / REPL loop, one frame-unit per iteration |
| WASM     | Real-time playback synced to the browser's render | Host-clock-driven                     | `scheduler_main_loop` per `requestAnimationFrame` |

### 10.1 The VBL frame-unit

`scheduler_run_frame(s, config)`
([scheduler.c](../src/core/scheduler/scheduler.c)) is the atomic step:

1. `trigger_vbl(config)` — pulse the machine's VBL line (VIA CA1, `image_tick_all`, …).
2. `scheduler_run(s, MAC_VBL_PERIOD)` — run exactly one VBL period of emulated time.

So the VBL is **not** a scheduler event; it is injected imperatively, once at the start
of every frame-unit. A run is just a sequence of frame-units:

```
[VBL, run 1/60.15 s] [VBL, run 1/60.15 s] [VBL, run 1/60.15 s] …
```

This is the sense in which **VBLs define frames** (see "Common misconceptions"): the
loop drives VBLs, and the emulated time between two VBLs is one frame. Every target
sees this identical sequence; only the wall-clock spacing between iterations differs.

### 10.2 Headless: unthrottled, one frame-unit at a time

Headless does not arm any VBL event. Its run loops
([headless_main.c](../src/platform/headless/headless_main.c)) call
`scheduler_run_frame()` back-to-back as fast as the host CPU allows:

- the daemon/script pump (`pump_scheduler_with_heartbeat`) runs one frame-unit per
  iteration, yielding between them only to emit the ~1 Hz heartbeat and poll the daemon
  socket for disconnect/`stop`;
- the interactive REPL loop runs one frame-unit per iteration, staying responsive to
  Ctrl-C and the `--max-cycles` cap.

An instruction-budget `scheduler.run N` schedules a `run_stop_event`; the inner
`scheduler_run` inside a frame-unit clamps to it (§6.1), so the budget stops mid-frame
at exactly `N` instructions and the loop exits. `scheduler.run` with no argument runs
until `scheduler.stop` (or client disconnect) — exactly what web2 does.

No `host_time()` value ever feeds guest execution on the headless path.

### 10.3 WASM: host-clock-driven, two pacing modes

`scheduler_main_loop(config, now_msecs)` is called once per `requestAnimationFrame`
([em_main.c](../src/platform/wasm/em_main.c)). It maps the elapsed host time onto a
whole number of frame-units and runs that many via `scheduler_run_frame()`:

- **`schedule_paced`** (default; "Real-Time" in the web2 toolbar): a wall-clock
  accumulator. Elapsed host time accumulates in `vbl_acc_error`; each whole
  `MAC_VBL_PERIOD` earns one frame-unit, capped at `PACED_MAX_CATCHUP` (4) per tick.
  At a ~60 Hz host this is 1 frame-unit per tick in steady state (one repeat/skip
  every ~7 s of oscillator drift — imperceptible); on 59.94/75/120/144 Hz and VRR
  displays the long-term rate converges to 60.147 Hz exactly. The cap prevents the
  catch-up death-spiral on a slow host: without it, `floor(vbl_acc_error / P)` is
  unbounded and each oversized burst lengthens the next tick's period — positive
  feedback that could pile ~60 frame-units into one tick. Under a sustained-slow host
  the accumulator saturates and the emulator simply lags real time by a bounded
  amount.
- **`schedule_accelerated`** ("Accelerated"): shares the paced wall-clock
  accumulator verbatim — same `vbl_acc_error` math, same `PACED_MAX_CATCHUP` cap —
  so its timebase (VBL cadence, and everything cycle-derived) is real-time exactly
  like paced. It differs from paced only in the effective CPI *inside* each
  frame-unit (§2.2): more instructions per frame, correct clock. This is the
  "faster machine, correct time" mode a CPU-bound title (e.g. Marathon's software
  renderer) wants. By default the adaptive governor (§2.3) picks the speed from
  the measured per-frame host cost, evaluated right here in the main loop's
  timing-update tail.
- **`schedule_unthrottled`** ("Fast-Forward"): as many frame-units as fit in
  `TURBO_HOST_HEADROOM` (50%) of the smoothed host loop period, leaving the rest for
  the browser. Emulated time outruns real time on a fast host. The first tick after
  init / checkpoint restore / mode switch has no `host_secs_per_vbl` estimate yet
  (it's `NAN`) and is explicitly guarded to run exactly 1 frame-unit — a float→int
  conversion of NaN is undefined behaviour.

A smoothed EWMA of host seconds per VBL/loop drives the turbo heuristic; a >1 s gap
(tab backgrounded) resets rather than fast-forwarding. `scheduler_set_mode` resets
the estimators and the accumulator on every switch, so turbo-shaped estimates never
leak into paced pacing.

> **History.** There used to be a third mode: `schedule_real_time` mapped host ticks
> 1:1 onto frame-units when the host loop period was within ±50% of the VBL period,
> falling back to the accumulator otherwise. The 1:1 branch carried a fixed 0.25–0.34%
> rate bias at ~60 Hz hosts (right at the edge of the audio rate-trim window) and
> already abandoned 1:1 on high-refresh displays; the accumulator subsumes it. The
> old `hw_accuracy` mode's real content was the CPI, which is now per-machine (§2.1).

### 10.4 Determinism

Because the frame-unit sequence is fixed — a constant number of instructions per
frame-unit, with the VBL always at the frame boundary — guest state at a given
instruction budget is a pure function of the budget, on **both** targets. Pacing only
changes *how many* frame-units a single host tick batches and *when* you observe the
result; it never changes the guest sequence or the stop point.

Headless makes this directly usable: a fixed `scheduler.run N` lands on the same guest
state every run, regardless of host load or where read-only commands (`screenshot`,
`screen.checksum`) sit in the script. The web2 e2e login test relies on the same
property — a fixed budget reproduces, bit-for-bit, the framebuffer the headless
integration test captures. With CPI fixed per machine (§2.1) this holds in `paced`
and `unthrottled` alike: paced web2, turbo web2, and headless reproduce each other
exactly at any given budget.

**`accelerated` is non-deterministic by construction** and excluded from this
guarantee: its guest timeline depends on the speed setting (and, once the adaptive
governor ships, on host load). Never pin an instruction budget or a checkpoint-replay
expectation against accelerated mode — determinism-sensitive consumers (fixed-budget
tests, reproduction) use `paced` with the authentic CPI.

> **History.** Earlier, headless used a *cycle-driven* recurring `scheduler_vbl_tick`
> event (and `scheduler_run_until_idle`) while WASM used `scheduler_main_loop`. The two
> VBL cadences differed in phase, so a boot could behave differently on the two targets
> — headless could mask a WASM-only bug (it did, for the IIfx 8bpp `f_trap` double-fault
> hang). Unifying on the frame-unit removed that divergence: headless now reproduces the
> web2 boot exactly, differing only in pacing.

---

## 11. Checkpoint save/restore

### 11.1 Save (`scheduler_checkpoint`)

Two parts are written:

1. **Plain-data fields** — everything from the top of the struct up to but not
   including `event_types`, written in one block via `system_write_checkpoint_data`
   ([scheduler.c:629](../src/core/scheduler/scheduler.c#L629)).
2. **Event queue** — each event is converted to a checkpoint-friendly form
   (`event_as_checkpoint_t`) with `source_name` and `event_name` strings instead of
   raw pointers. The strings are looked up in the `event_types` registry, which must
   have been populated by device code via `scheduler_new_event_type`.

Events with no registered type cause a save-time assertion failure. This is the
mechanism that forces every device scheduling events to declare them by name.

### 11.2 Restore

Restore happens in two phases:

**Phase 1 (`scheduler_init` with a non-NULL checkpoint):**

- Plain-data fields are read back into the struct.
- Host-timing fields (`previous_time`, `vbl_acc_error`, `host_secs_per_vbl`,
  `host_secs_per_loop`) are re-initialized from the *current* host clock — they do not
  survive a restore.
- `total_instructions` is *reconstructed* from `cpu_cycles` using the restored mode's
  CPI (§9 caveat).
- Event data is read into `tmp_events`, a flat array. Pointers cannot be resolved yet
  because devices haven't registered their event types.

**Phase 2 (`scheduler_start`, called after all devices have booted and registered
their event types):**

- Each saved event is matched by `(source_name, event_name)` against the live
  `event_types` registry.
- A live `event_t` is malloc'd, populated with the resolved `source` and `callback`
  pointers, and inserted into `cpu_events` via `insert_event_queue`.
- `tmp_events` is freed.

Unresolved events (no matching type) cause a hard assert — a checkpoint with a stale
or misnamed event type cannot be silently dropped.

### 11.3 Cross-target checkpoints (headless ↔ WASM)

VBL is no longer a scheduler event on any target (§10) — it is injected imperatively
per frame-unit — so no checkpoint's queue ever contains a `vbl_tick` entry, and there
is no VBL-source mismatch to resolve. Both targets save and restore the same set of
event types (VIA timers, SCC, the transient `run_stop`, …), so a checkpoint's event
queue is target-agnostic with respect to VBL.

---

## 12. Invariants

Enforced by `scheduler_check_invariants` at every API entry/exit:

**Sprint counters:**
- `sprint_burndown <= sprint_total` (at all times)
- `sprint_total == 0` between sprints
- The derived quantities `current_cpu_cycles(s)` and `cpu_instr_count()` are monotonic
  across reconciliation and sprint boundaries.

**Cycle counter:**
- `cpu_cycles` is monotonically non-decreasing.
- `cpu_cycles < 2^60` (sanity bound — ~4.7 million years at 7.8 MHz).

**Event queue:**
- Non-null callbacks on every event.
- Non-decreasing timestamps along the list.
- First event's timestamp satisfies the relaxed past bound: `timestamp + CPI >=
  cpu_cycles` (§8.2).
- Queue length ≤ `MAX_SANE_EVENTS` (10000) — a loop/corruption tripwire.

**Mode and pointers:**
- `mode` is one of the three enum values (`schedule_paced` / `schedule_unthrottled` /
  `schedule_accelerated`).
- `cpi_eff_x256` is nonzero and never above the authentic `cpi << 8`;
  `cycle_frac_x256 < 256`.
- `cpu` pointer is non-NULL.

All violations abort via `GS_ASSERT` / `GS_ASSERTF` (from `common.h`) with file, line,
function, and context. There are no `printf`-based error reports in the scheduler.

---

## 13. Shell surface

The scheduler is an object-model citizen (`scheduler.*` paths in the typed shell):

| Path                        | Description                                                |
|-----------------------------|------------------------------------------------------------|
| `scheduler.run [N]`         | Start execution; optionally stop after N instructions.     |
| `scheduler.stop`            | Stop execution immediately.                                |
| `scheduler.mode`            | Pacing mode: `"paced"` \| `"accelerated"` \| `"turbo"` (writable; legacy aliases `real`/`hw` → paced, `max` → turbo, `accel` → accelerated). |
| `scheduler.cpi`             | Per-machine CPI constant; writable as a debug override (1..255). |
| `scheduler.speed`           | Accelerated-mode multiplier in force (live). Write `0` for auto (adaptive governor) or 1.0 .. 8.0 to pin; persisted, but only takes effect in mode `accelerated`. |
| `scheduler.speed_auto`      | RO: true while the adaptive governor is choosing the speed (`speed = 0`). |
| `scheduler.max_speed`       | Cap on the accelerated multiplier (1.0 .. 8.0, default 8): the governor's ceiling, and pinned speeds clamp to it. Persisted. |
| `scheduler.running`         | True while executing (useful for scripts).                 |
| `scheduler.cycles` / `.instr_count` | Cycle / instruction counters.                      |
| `events`                    | Dump the pending event queue with Δcycles and Δµs.         |

The `events` command is the quickest way to diagnose a timing issue — it shows each
event's absolute timestamp, delta from `cpu_cycles`, delta in microseconds, its
`source_name.event_name` (via the event-type registry), and its `data` payload.
