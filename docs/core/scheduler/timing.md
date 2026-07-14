# Timing Model

This document describes how Granny Smith tracks emulated time, including the
relationship between CPU instructions, clock cycles, and scheduled events. The
model applies to all supported machines (Plus, SE/30, IIcx, etc.) though each
machine provides its own clock frequency and per-device tuning parameters.

## Overview

Emulated time is measured in **CPU clock cycles**. The scheduler maintains an
authoritative cycle counter (`cpu_cycles`) that advances as the CPU executes
instructions. All event timestamps, timer deadlines, and host-time
synchronization are expressed in this cycle domain.

Instructions are not timed individually. Instead, the emulator uses an **average
cycles-per-instruction (CPI)** value: each instruction is assumed to cost a
fixed number of cycles. This is a deliberate simplification — real 68000/68030
instructions have widely varying execution times — but it is sufficient for
faithful emulation because the CPI value is tuned to produce the correct
aggregate throughput and the system is not cycle-exact.

On top of this base CPI model, the emulator supports **I/O bus cycle
penalties**: extra cycles charged when the CPU accesses slow I/O devices. These
penalties are applied per-access in the memory slow path and are invisible to
the RAM/ROM fast path.

## Cycles-Per-Instruction (CPI)

Each machine configures **one** CPI constant at initialization via
`scheduler_set_cpi(s, cpi)`; `avg_cycles_per_instr(s)` returns it unchanged in
every pacing mode. Because CPI never varies at runtime (short of the
`scheduler.cpi` debug override), the relationship
`total_instructions * CPI == cpu_cycles` holds globally and the guest's
execution timeline is a pure function of the frame-unit count — identical in
both pacing modes and on both targets.

Current values: the Plus uses 12 (the authentic average for its 7.8336 MHz
68000); the 030 machines use 4 (4-clock bus cycle at 15.6672 MHz with
1-wait-state RAM); the Lisa/Mac XL uses 4 (its long-standing effective value).

## The Sprint Model

CPU execution is organized into **sprints** — bursts of N instructions executed
without returning to the scheduler. Sprints are the fundamental unit of
work scheduling.

### Sprint Lifecycle

```
1. PLAN      Calculate cycle budget from remaining work and next event
2. SIZE      Convert cycle budget to instructions: N = budget / CPI
3. SETUP     sprint_total = sprint_burndown = N
4. EXECUTE   cpu_run_sprint(cpu, &sprint_burndown)
             CPU decoder loop decrements sprint_burndown per instruction
5. ACCOUNT   executed_cycles = sprint_total * CPI
             cpu_cycles += executed_cycles
             total_instructions += real instructions executed
6. EVENTS    Fire any events with timestamp <= cpu_cycles
```

### Sprint Planning and Events

When a sprint begins, the scheduler calculates how many cycles it can execute
before the next pending event:

```c
cycles_to_event = event->timestamp - cpu_cycles;
cycles_to_execute = MIN(cycles_to_event, remaining_cycles);
instr_to_exec = cycles_to_execute / CPI;
```

This ensures the sprint ends at or just before the event's timestamp. After the
sprint, `cpu_cycles` is advanced and due events are processed.

### Mid-Sprint Queries

During a sprint, `current_cpu_cycles()` returns the correct "now" including
in-progress execution:

```c
current_cpu_cycles(s) = cpu_cycles + (sprint_total - sprint_burndown) * CPI
```

This is used when device writes trigger mid-sprint event scheduling (e.g., a
VIA timer reload). The scheduler's `reconcile_sprint()` stabilizes the counters
before inserting the event, then the sprint terminates early.

### Instruction Atomicity and Event Overshoot

Instructions are atomic — the emulator cannot stop mid-instruction. When the
cycle budget is not an exact multiple of CPI, the sprint must execute at least
one instruction, potentially overshooting the target by up to CPI-1 cycles.

The scheduler handles this by:

1. Processing all events with `timestamp <= cpu_cycles` (including slightly
   overshot ones)
2. Allowing up to CPI cycles of overshoot in invariant checks
3. Letting event callbacks schedule new events while overshot events remain
   in the queue

## Event Scheduling

Events are the mechanism by which time-dependent hardware behaviour is
modelled: VIA timers, VBL interrupts, ADB polling, sound buffer refills, and
so on.

### Event Structure

Each event carries an absolute cycle timestamp:

```c
struct event {
    event_t *next;              // linked-list pointer (sorted queue)
    uint64_t timestamp;         // absolute cycle count when event fires
    event_callback_t callback;  // function to call
    void *source;               // device that owns this event
    uint64_t data;              // opaque payload
};
```

### Scheduling an Event

Events are created via `scheduler_new_cpu_event()`, which accepts a delay in
either cycles or nanoseconds (exactly one must be non-zero). Nanosecond delays
are converted to cycles using the machine's clock frequency:

```c
cycles = ns * frequency / 1e9
```

The event's absolute timestamp is `current_cpu_cycles() + delay_cycles`. The
event is inserted into a sorted linked-list priority queue.

### Event Processing

At the end of each sprint, the scheduler processes all due events:

```c
while (first_event->timestamp <= cpu_cycles) {
    remove event from queue
    call event->callback(source, data)
    // callback may schedule new events
}
```

Events are one-shot. Periodic behaviour (e.g., VIA Timer 1 auto-reload) is
implemented by the callback scheduling a new event before returning.

## I/O Bus Cycle Penalties

Real hardware imposes additional bus wait states when the CPU accesses I/O
devices. For example, on the SE/30, VIA register accesses take ~19-23 CPU
clocks (due to E-clock synchronization) versus 4 clocks for RAM. The emulator
models this through **I/O cycle penalties** — extra cycles charged per I/O
access that are invisible to the RAM/ROM fast path.

### The Problem: Sprint Budget Mismatch

Without I/O penalties, each instruction costs exactly CPI cycles, and sprints
are sized to consume a precise cycle budget. I/O penalties break this
assumption: a sprint sized for 400 cycles of work might actually consume 416
cycles if one instruction reads a VIA register (adding 16 penalty cycles). This
would cause the sprint to overshoot the next event.

### The Solution: Phantom Instructions

I/O penalties are converted into **phantom instructions** that consume sprint
burndown slots. A phantom instruction does not correspond to a real CPU
instruction — it represents bus stall time during which the CPU is waiting for
the device to respond (DSACK held by the address decoder ASIC).

When an I/O device handler calls `memory_io_penalty(extra_cycles)`:

1. The penalty cycles are added to a sub-CPI remainder accumulator
2. When the remainder reaches one CPI's worth, a phantom instruction is
   "burned" — `sprint_burndown` is decremented by 1
3. The phantom instruction counter (`g_io_phantom_instructions`) is incremented

This causes the sprint to end sooner: the CPU executes fewer real instructions,
but the total "slots" consumed (real + phantom) still match the cycle budget.

### Arithmetic

With CPI = 4 and a 16-cycle VIA penalty:

```
Sprint: 100 instruction slots (400 cycle budget)
  Instruction 30: VIA read → 16 extra cycles → 4 phantom instructions
  sprint_burndown reduced by 4 → CPU executes 66 more real instructions
  Sprint ends: 96 real + 4 phantom = 100 slots
  executed_cycles = 100 * 4 = 400 → event fires on time
```

The identity holds: `(real + phantom) * CPI = real_cycles + penalty_cycles`.

When the penalty is not evenly divisible by CPI (e.g., 19 cycles at CPI=4),
the remainder (3 cycles) carries to the next I/O access. Over time, the
accumulator ensures exact long-term cycle accounting with per-access jitter of
at most CPI-1 cycles — the same magnitude as the existing instruction atomicity
tolerance.

### Integration with the Memory Subsystem

The penalty mechanism exploits the existing two-tier memory architecture:

- **Fast path** (RAM/ROM): SoA array entry is non-zero. Inline accessor
  returns immediately. No penalty code is reached.
- **Slow path** (I/O devices): SoA entry is zero (set by `memory_map_add()`).
  Dispatches to device handler. The machine's I/O dispatcher calls
  `memory_io_penalty()` with a device-specific penalty value.

Because I/O pages always have zero SoA entries, the penalty mechanism adds
**zero overhead** to the RAM/ROM hot path. The `memory_io_penalty()` inline
function is never even compiled into the fast-path code flow.

### State and Globals

The penalty mechanism uses four globals, all defined in `memory.c`:

| Global | Type | Description |
|--------|------|-------------|
| `g_io_penalty_remainder` | `uint32_t` | Sub-CPI fraction carried across sprints |
| `g_io_phantom_instructions` | `uint32_t` | Phantom instructions consumed this sprint |
| `g_io_cpi` | `uint32_t` | Current CPI for conversion; 0 = disabled |
| `g_sprint_burndown_ptr` | `uint32_t *` | Points to scheduler's `sprint_burndown` during sprint |

The scheduler sets `g_io_cpi` and `g_sprint_burndown_ptr` at sprint start and
clears `g_sprint_burndown_ptr` at sprint end. `g_io_penalty_remainder` is not
reset at sprint boundaries — it carries across sprints to preserve sub-CPI
fractions. `g_io_phantom_instructions` is reset at each sprint start and
harvested at sprint end.

Setting `g_io_cpi = 0` disables the entire mechanism. This happens implicitly
when no I/O penalties are configured. Do **not** use it to disable penalties in
one pacing mode but not the other — I/O penalties are part of the guest
timeline, and gating them on the mode would break the one-guest-timeline
property (identical execution in paced/turbo/headless at a given budget).

### Configuring Per-Device Penalties

Each machine defines its own I/O penalty constants. The penalty value represents
**extra CPU clocks per byte access** beyond the CPI baseline (which already
accounts for a RAM-speed bus cycle). For example:

```c
// Extra clocks per byte: total device clocks minus RAM baseline
#define MACHINE_VIA_IO_PENALTY   16   // VIA: ~20 avg - 4 RAM baseline
#define MACHINE_SCC_IO_PENALTY    2   // SCC: ~6 - 4
```

These are passed to `memory_io_penalty()` in the machine's I/O dispatcher. The
16-bit and 32-bit I/O dispatch paths typically delegate to the 8-bit path, so
wider accesses automatically accumulate per-byte penalties (matching real
hardware's dynamic bus sizing for 8-bit devices). For coalesced paths (e.g.,
SCSI pseudo-DMA longword reads), the penalty is multiplied by the byte count
explicitly.

### Post-Sprint Accounting

After a sprint completes, the scheduler separates real from phantom
instructions:

```c
executed_slots = sprint_total;          // real + phantom
phantom = g_io_phantom_instructions;
real_instructions = executed_slots - phantom;

executed_cycles = executed_slots * CPI;  // correct: includes penalty time
total_instructions += real_instructions; // accurate instruction count
cpu_cycles += executed_cycles;
```

`total_instructions` reflects only genuinely executed CPU instructions.
`cpu_cycles` reflects the full time cost including I/O stalls.

## Scheduler Modes

The scheduler has two **pacing** modes that decide how host wall-clock time
maps to frame-units. The modes are only relevant to the WASM target — the
headless target ignores host time entirely (see "Target-Specific Pacing"
below).

| Mode | Behaviour (WASM target) |
|------|-------------------------|
| `schedule_paced` (default; "Live") | Wall-clock accumulator: one frame-unit per accumulated VBL period, capped at `PACED_MAX_CATCHUP` (4) per host tick. Long-term rate converges to 60.147 Hz on any display refresh rate. |
| `schedule_unthrottled` ("Turbo") | As many frame-units as fit in `TURBO_HOST_HEADROOM` (50%) of the host loop period — as fast as the host allows. |

Both modes use the same sprint and event machinery, the same per-machine CPI,
and the same I/O penalties. The mode affects **only** how many frame-units a
host tick batches — the guest's execution sequence at a given frame-unit count
is identical in both modes (and matches headless).

## Target-Specific Pacing

Both targets run the **same** execution model — a sequence of *VBL frame-units*,
each `scheduler_run_frame()`: pulse the VBL line (`trigger_vbl()`), then run one
VBL period (~16.63 ms) of guest time. They differ only in **pacing** — how fast
the run loop issues frame-units.

### WASM target: host-clock-driven, real-time pacing

`scheduler_main_loop()` is called from the WASM frontend once per
`requestAnimationFrame` (~60 Hz). Each call measures elapsed host time, decides
how many frame-units to run this tick (per the current pacing mode), and runs
that many via `scheduler_run_frame()`. In `schedule_paced` this keeps the
emulated machine synced to wall-clock (browser render rhythm); in
`schedule_unthrottled` it fills half of each tick with emulation.

### Headless target: unthrottled, one frame-unit at a time

Headless arms no VBL event. Its run loops call `scheduler_run_frame()`
back-to-back as fast as the host allows, yielding between frame-units only for
the heartbeat / daemon-poll (pump) or Ctrl-C / `--max-cycles` (REPL). An
instruction-budget `scheduler.run N` schedules a `run_stop_event` that the inner
`scheduler_run` clamps to, stopping mid-frame at exactly `N`; no argument runs
until `scheduler.stop`. No `host_time()` feeds guest execution.

Property: a frame-unit is a constant number of instructions with the VBL at its
boundary, so guest state at a given budget is a pure function of the budget on
both targets. Headless makes this directly usable — a fixed `scheduler.run N`
yields identical guest state (and screenshots) every run, regardless of script
shape or where read-only commands like `screenshot` sit. Pacing changes only how
many frame-units a host tick batches and when you observe, never the sequence.

See `docs/core/scheduler/scheduler.md` §10 for the full design.

## Clock Frequencies

Each machine sets its CPU clock frequency via `scheduler_set_frequency()`:

| Machine | CPU | Frequency | Default CPI (hw/fast) |
|---------|-----|-----------|----------------------|
| Plus | 68000 @ 7.8336 MHz | 7,833,600 Hz | 12 / 4 |
| SE/30 | 68030 @ 15.6672 MHz | 15,667,200 Hz | 4 / 4 |
| IIcx | 68030 @ 15.6672 MHz | 15,667,200 Hz | 4 / 4 |

The frequency is used to convert between cycles and wall-clock time (for
nanosecond-based event scheduling and the `scheduler_time_ns()` query).

## Consistency Invariants

The scheduler enforces these invariants at key transition points:

### Sprint Counters

1. `sprint_burndown <= sprint_total` (always)
2. `sprint_total == 0` between sprints
3. `sprint_total > 0 && sprint_burndown >= 0` during sprints

### Cycle Counter

1. `cpu_cycles` is monotonically increasing
2. All events in the queue have `timestamp >= cpu_cycles`
3. Events are sorted by increasing timestamp
4. Relaxed: first event's timestamp + CPI >= cpu_cycles (allows instruction
   atomicity overshoot)

### Event Queue

1. Every event has a non-NULL callback
2. Queue length is bounded (sanity-checked at 10,000)

## Checkpoint Interaction

The scheduler saves its plain-data fields (including `cpu_cycles`, `mode`,
`total_instructions`, the CPI constant) to checkpoints. Event queues are
serialized using registered event type names rather than function pointers.
(Checkpoint files are gated on an exact build-ID match, so a checkpoint never
crosses builds — pre-two-modes checkpoints with the old three-value mode enum
and dual CPI fields are rejected by that gate, not migrated.)

On restore, `total_instructions` is reconstructed as `cpu_cycles / cpi` — exact,
since CPI is constant. Sprint counters are reset to zero. The I/O
penalty remainder (`g_io_penalty_remainder`) is a global carried across sprints
and should be included in checkpoint save/restore for full accuracy, though its
magnitude is bounded by CPI-1 and the impact of losing it is negligible.

## Key Files

| File | Purpose |
|------|---------|
| `src/core/scheduler/scheduler.h` | Public API: event management, time queries, mode control |
| `src/core/scheduler/scheduler.c` | Sprint loop, event queue, cycle accounting, VBL timing |
| `src/core/memory/memory.h` | `memory_io_penalty()` inline, penalty globals |
| `src/core/memory/memory.c` | Penalty global definitions, slow-path device dispatch |
| `src/machines/se30.c` | SE/30 I/O dispatcher with per-device penalty constants |
| `docs/core/scheduler/scheduler.md` | Detailed scheduler internals (sprint model, event lifecycle) |

## See Also

- `docs/core/scheduler/scheduler.md` — Sprint model internals, event queue details, assertion
  strategy
- `docs/core/memory/memory.md` — Memory subsystem architecture, fast/slow path, MMU
  integration
