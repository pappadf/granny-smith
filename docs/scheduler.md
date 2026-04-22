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
4. **Pace real-time playback** against the host wall clock so the emulated Mac feels
   like a real Mac (roughly 60 VBL/s) without starving the host.

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

- **"Switching scheduler mode (max/real/hw) rebases cycles to match instructions."**
  No. `cpu_cycles` is invariant across mode changes. Only the cycles-per-instruction
  ratio changes, which means the instruction count and cycle count *diverge* from that
  point on. There is no invariant `cpu_instr_count * CPI == cpu_cycles` — see §9.

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
current cycles-per-instruction (CPI) ratio, and they are *not* cycle-perfect — the
emulator models "average" CPI, not per-opcode cycle timings.

### 2.1 Cycles-per-instruction (CPI)

CPI depends on the scheduler mode and is configurable per machine:

| Mode                  | Default CPI | Notes                                       |
|-----------------------|-------------|---------------------------------------------|
| `schedule_hw_accuracy`| `cpi_hw` = 12  | Hardware-accurate pacing                 |
| `schedule_real_time`  | `cpi_fast` = 4 | Real-time (wall-clock-synced) pacing     |
| `schedule_max_speed`  | `cpi_fast` = 4 | As fast as the host can go               |

Defaults are set in [scheduler.c:30-31](../src/core/scheduler/scheduler.c#L30) and can be
overridden per-machine via `scheduler_set_cpi(s, cpi_hw, cpi_fast)`. The shell command
`schedule cpi <N>` changes CPI for the currently active mode.

**Key consequence:** CPI can change at runtime. This means the relationship between
cycles and instructions is not linear over time — see §9 for the implications.

---

## 3. Core data structures

### 3.1 The scheduler struct

The entire scheduler state is encapsulated in one opaque struct
([scheduler.c:68-100](../src/core/scheduler/scheduler.c#L68)). The fields that matter
for timing:

```c
struct scheduler {
    enum schedule_mode mode;         // Selects CPI
    uint32_t cpi_hw, cpi_fast;       // Per-machine CPI values
    uint32_t frequency;              // CPU clock in Hz

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

Assume CPI=4, we are in `schedule_real_time`. The scheduler finished its last sprint at
cycle 1,000 having retired 250 instructions. State:

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
    g_io_cpi = avg_cycles_per_instr(s);
    g_io_phantom_instructions = 0;
    cpu_run_sprint(cpu, &sprint_burndown);                       // CPU runs until burndown hits 0
    g_sprint_burndown_ptr = NULL;

    // 3. Finalize: fold the sprint's work into the authoritative counters
    executed_slots = sprint_total;                               // may have been shrunk by reconcile
    phantom = g_io_phantom_instructions;
    sprint_total = 0;
    executed_cycles = executed_slots * avg_cycles_per_instr(s);

    total_instructions += (executed_slots - phantom);            // only real instructions count
    cpu_cycles += executed_cycles;                               // but all cycles count
    remaining_cycles -= executed_cycles;

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
// memory.h:142 — memory_io_penalty
g_io_penalty_remainder += extra_cycles;
burn = g_io_penalty_remainder / g_io_cpi;
if (burn > 0) {
    g_io_penalty_remainder -= burn * g_io_cpi;
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
- Switch the machine to `schedule_hw_accuracy` mode with a realistic per-instruction
  CPI so `N` is measured in cycles, not coarse units.
- Re-read the current time inside the callback via `scheduler_cpu_cycles(s)` and
  compensate (most VIA/SCC device models already do this for edge timings).

---

## 9. Mode switching and why there is no CPI invariant

The following is **not** an invariant and must not be assumed:

```
cpu_instr_count() * avg_cycles_per_instr(s) == cpu_cycles    // WRONG
```

`scheduler_set_mode` changes the CPI immediately but leaves `cpu_cycles` and
`total_instructions` untouched
([scheduler.c:862-870](../src/core/scheduler/scheduler.c#L862)). From that moment on,
future instructions advance `cpu_cycles` at the new rate. The relationship between
total instructions and total cycles is *piecewise* linear, not globally linear.

The authoritative quantity is always `cpu_cycles`. If you need to know elapsed emulated
time, use cycles (or `scheduler_time_ns`). Don't reconstruct time from instruction
counts.

**Checkpoint restore caveat:** The restore path rebuilds `total_instructions` from the
stored `cpu_cycles` using the *restored mode's* CPI
([scheduler.c:552-553](../src/core/scheduler/scheduler.c#L552)). The resulting value is
therefore an approximation — it is exact only if CPI was constant between boot and the
checkpoint. This is acceptable because `total_instructions` is used only for display
and debugging, not for timing.

---

## 10. Real-time pacing: the main loop

`scheduler_main_loop(config, now_msecs)` is called from the host platform's render
loop (once per `requestAnimationFrame` in the browser, or per host VBL in the headless
build). It decides how many emulated VBLs to run this host tick, then runs them via
`scheduler_run(s, MAC_VBL_PERIOD)`.

Per-mode behavior
([scheduler.c:1060-1087](../src/core/scheduler/scheduler.c#L1060)):

- **`schedule_max_speed`**: execute as many VBLs as fit in ~50% of the smoothed host
  loop period. Emulated time runs faster than real time if the host is fast.
- **`schedule_real_time`**: execute 1 VBL per host tick if the host loop period is
  close to the Mac VBL period (±50%). Otherwise fall through to the accumulator path.
- **`schedule_hw_accuracy`**: accumulate host elapsed time in `vbl_acc_error` and
  execute `floor(vbl_acc_error / MAC_VBL_PERIOD)` emulated VBLs when enough time has
  piled up. This keeps emulated time exactly in step with wall-clock time over the long
  run.

A smoothed EWMA of host seconds per VBL (`host_secs_per_vbl`) and per loop
(`host_secs_per_loop`) drives the scheduling heuristics. If the host tab is
backgrounded for more than 1 second, the accumulator is reset to avoid fast-forwarding.

Within each emulated VBL, `scheduler_run` computes an instruction budget for the VBL
period and feeds it to `scheduler_run_instructions`, which runs the sprint loop
described in §6.

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
- `mode` is one of the three enum values.
- `cpu` pointer is non-NULL.

All violations abort via `GS_ASSERT` / `GS_ASSERTF` (from `common.h`) with file, line,
function, and context. There are no `printf`-based error reports in the scheduler.

---

## 13. Shell commands

Registered in `scheduler_init` ([scheduler.c:576-581](../src/core/scheduler/scheduler.c#L576)):

| Command                  | Description                                            |
|--------------------------|--------------------------------------------------------|
| `run [instructions]`     | Start execution; optionally stop after N instructions. |
| `stop`                   | Stop execution immediately.                            |
| `schedule`               | Show current mode and CPI.                             |
| `schedule max\|real\|hw` | Switch mode.                                           |
| `schedule cpi <N>`       | Override CPI for the current mode (1..255).            |
| `status`                 | Return 1 if running, 0 if idle (useful for scripts).   |
| `events`                 | Dump the pending event queue with Δcycles and Δµs.     |

The `events` command is the quickest way to diagnose a timing issue — it shows each
event's absolute timestamp, delta from `cpu_cycles`, delta in microseconds, its
`source_name.event_name` (via the event-type registry), and its `data` payload.
