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

Each machine configures two CPI values at initialization:

| Field | Usage |
|-------|-------|
| `cpi_hw` | Used in `schedule_hw_accuracy` mode |
| `cpi_fast` | Used in `schedule_real_time` and `schedule_max_speed` modes |

The active CPI is selected by `avg_cycles_per_instr(s)` based on the current
scheduler mode. It can change at runtime when the user switches modes, creating
a discontinuity in the instruction-to-cycle relationship. This is why
`total_instructions * CPI != cpu_cycles` in general — it only holds when CPI
has been constant since boot.

Machines set their CPI values via `scheduler_set_cpi()` during initialization.
For example, the SE/30 sets both to 4 (matching a 4-clock bus cycle at
15.6672 MHz with 1-wait-state RAM), while the Plus defaults to 12/4
(hw_accuracy/fast).

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
when no I/O penalties are configured, and can be used to disable penalties in
`schedule_max_speed` mode for raw throughput.

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

The scheduler supports three execution modes that affect how host wall-clock
time maps to emulated time. The modes are only relevant to the WASM target —
the headless target ignores host time entirely (see "Target-Specific Pacing"
below).

| Mode | CPI | Behaviour (WASM target) |
|------|-----|-------------------------|
| `schedule_max_speed` | `cpi_fast` | Execute as many VBLs as fit in ~50% of host loop period |
| `schedule_real_time` | `cpi_fast` | One-to-one VBL mapping when host loop is close to VBL period |
| `schedule_hw_accuracy` | `cpi_hw` | Accumulate elapsed host time, execute proportional VBLs |

All three modes use the same sprint and event machinery. The mode only affects:

1. Which CPI value is used for the instruction-to-cycle conversion
2. How many VBL intervals are executed per host loop iteration (WASM target)
3. Whether I/O penalties are active (configurable per machine)

## Target-Specific Pacing

The two platform targets pace execution differently because they have
different goals.

### WASM target: host-clock-driven, real-time pacing

`scheduler_main_loop()` is called from the WASM platform frontend once per
`requestAnimationFrame` (~60 Hz). Each call:

1. Measures elapsed host time since the last call
2. Determines how many VBL intervals to execute based on the current mode
3. For each VBL: calls `trigger_vbl()` (which pulses VIA CA1 etc.), then
   runs the scheduler for one VBL period (~16.63 ms) of guest time

This keeps the emulated machine synced to the browser's render rhythm. It is
**not** byte-deterministic — VBL count tracks host load — and that is
intentional for an interactive emulator.

### Headless target: cycle-driven, run as fast as possible

The headless platform calls `scheduler_start_vbl(scheduler, config)` once
after machine setup, registering a recurring `scheduler_vbl_tick` event on
the cycle queue at `frequency / MAC_VBL_FREQUENCY` cycles in the future. The
callback calls `trigger_vbl()` and reschedules itself. VBL is then just
another event on the same priority queue as VIA timers and SCC completions.

Execution is driven by `scheduler_run_until_idle()`, which pumps
`scheduler_run_instructions` chunks until `s->running` goes false (typically
when `run_stop_event` from a `run N` shell command fires). No `host_time()`
is called on the headless execution path.

Property: because every input to guest execution is a function of cumulative
cycles, the headless target is byte-deterministic. Two scripts that produce
the same final cycle count produce identical guest state — including
identical screenshots — regardless of script shape (`run 200M` vs four
`run 50M` calls) or where read-only commands like `screenshot` are inserted.

See `docs/scheduler.md` §10 for the full design.

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
`total_instructions`, CPI values) to checkpoints. Event queues are serialized
using registered event type names rather than function pointers, allowing
restoration across builds.

On restore, `total_instructions` is reconstructed from `cpu_cycles / CPI` using
the restored mode's CPI value. Sprint counters are reset to zero. The I/O
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
| `docs/scheduler.md` | Detailed scheduler internals (sprint model, event lifecycle) |

## See Also

- `docs/scheduler.md` — Sprint model internals, event queue details, assertion
  strategy
- `docs/memory.md` — Memory subsystem architecture, fast/slow path, MMU
  integration
