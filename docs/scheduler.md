# Scheduler: Cycle and Time Bookkeeping

This document describes the internal timing model of the Granny Smith scheduler, including cycle counting, sprint
execution, event scheduling, and consistency guarantees.

## Overview

The scheduler manages emulated CPU time through a cycle-based timing model. All timing in the emulator is ultimately
measured in **CPU cycles**, which correspond to the Macintosh's 7.8336 MHz 68000 processor clock.

## Key Data Structures

### Scheduler Structure

All scheduler state — including sprint execution counters — is encapsulated in a single opaque struct:

```c
struct scheduler {
    enum schedule_mode mode;      // Affects cycles-per-instruction ratio
    bool running;                 // Whether the scheduler is actively executing
    uint64_t cpu_cycles;          // Authoritative cycle counter (updated at sprint boundaries)
    double previous_time;         // Previous host time (seconds)
    double vbl_acc_error;         // Accumulated VBL timing error (seconds)
    double host_secs_per_vbl;     // Smoothed host seconds per VBL
    double host_secs_per_loop;    // Smoothed host seconds per main loop iteration

    // Event type registry for checkpointing
    event_type_t event_types[MAX_EVENT_TYPES];
    int num_event_types;

    // Sprint execution counters
    uint64_t total_instructions;  // Accumulated instructions from completed sprints
    uint32_t sprint_total;        // Instructions planned for current sprint
    uint32_t sprint_burndown;     // Instructions remaining in current sprint

    struct cpu *cpu;              // CPU instance
    event_t *cpu_events;          // Priority queue of pending events (sorted by timestamp)

    // Temporary storage for checkpoint restore
    int tmp_num_events;
    event_as_checkpoint_t *tmp_events;

    uint32_t frequency;           // CPU clock frequency in Hz
};
```

Note: The sprint counters (`total_instructions`, `sprint_total`, `sprint_burndown`) were previously file-scope static
globals. They are now part of the scheduler struct for proper encapsulation.

## Cycles-Per-Instruction Ratio

The emulator uses an average cycles-per-instruction (CPI) value that depends on the scheduler mode:

| Mode | CPI | Description |
|------|-----|-------------|
| `schedule_hw_accuracy` | 12 | Hardware-accurate timing |
| `schedule_real_time` | 4 | Real-time synchronized |
| `schedule_max_speed` | 4 | Maximum speed |

**Important**: The CPI can change at runtime when the user switches modes. This means the relationship between
instruction count and cycle count is **not linear over time**.

## The Sprint Model

CPU execution is organized into **sprints** — bursts of instruction execution between event processing.

### Sprint Lifecycle

```
┌─────────────────────────────────────────────────────────────────┐
│                    SPRINT LIFECYCLE                              │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  1. SETUP: sprint_total = sprint_burndown = N                   │
│            (N = instructions to execute before next event)      │
│                                                                  │
│  2. EXECUTE: cpu_run_sprint(cpu, &s->sprint_burndown)           │
│              - CPU decrements sprint_burndown as it executes    │
│              - Memory writes may trigger event scheduling       │
│                                                                  │
│  3. FINALIZE: executed = sprint_total                           │
│               sprint_total = 0                                  │
│               total_instructions += executed                    │
│               cpu_cycles += executed * AVG_CYCLES_PER_INSTR     │
│                                                                  │
│  4. PROCESS EVENTS: Fire any events at or before cpu_cycles    │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Key Invariants During Sprint

At any point during execution, the total instruction count is:
```c
cpu_instr_count() = s->total_instructions + s->sprint_total - s->sprint_burndown
```

The "current time" in cycles is:
```c
current_cpu_cycles(s) = s->cpu_cycles + (sprint_total - sprint_burndown) * AVG_CYCLES_PER_INSTR
```

## Time Queries

### `cpu_instr_count()`

Returns the total number of instructions executed, including in-progress sprint. Internally fetches the scheduler via
`system_scheduler()`:
```c
uint64_t cpu_instr_count(void) {
    struct scheduler *s = system_scheduler();
    if (s == NULL) return 0;
    return s->total_instructions + s->sprint_total - s->sprint_burndown;
}
```

### `current_cpu_cycles(s)` (internal)

Returns the authoritative "now" time in cycles, including in-progress sprint:
```c
static uint64_t current_cpu_cycles(struct scheduler *s) {
    uint64_t in_sprint = s->sprint_total - s->sprint_burndown;
    return s->cpu_cycles + in_sprint * AVG_CYCLES_PER_INSTR;
}
```

### `scheduler_cpu_cycles(s)`

Public API for current time:
```c
uint64_t scheduler_cpu_cycles(struct scheduler *s) {
    return current_cpu_cycles(s);
}
```

## Event Scheduling

Events are scheduled relative to the current time (`current_cpu_cycles`).

### Event Structure

```c
struct event {
    event_t *next;           // Linked list pointer
    uint64_t timestamp;      // Absolute cycle count when event should fire
    event_callback_t callback;
    void *source;
    uint64_t data;
};
```

### Scheduling Flow

```
┌─────────────────────────────────────────────────────────────────┐
│               EVENT SCHEDULING FLOW                              │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  scheduler_new_cpu_event(scheduler, callback, source, data,     │
│                          cycles, ns)                             │
│      │                                                           │
│      ├── validate_cpu_events()     // Check queue integrity     │
│      │                                                           │
│      ├── reconcile_sprint()        // Stabilize sprint counters │
│      │       └── sprint_total = sprint_total - sprint_burndown  │
│      │       └── sprint_burndown = 0                            │
│      │                                                           │
│      └── add_event_internal()                                    │
│              │                                                   │
│              ├── now = current_cpu_cycles(scheduler)            │
│              │        = cpu_cycles + in_sprint * CPI            │
│              │                                                   │
│              ├── event->timestamp = now + cycles                │
│              │                                                   │
│              └── insert_event_queue() // Sorted by timestamp    │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Mid-Sprint Event Scheduling

When a memory write during CPU execution triggers event scheduling:

1. `reconcile_sprint()` is called to reconcile sprint counters
2. `current_cpu_cycles()` returns the correct "now" including in-sprint progress
3. Event timestamp is calculated relative to this accurate "now"

**Critical**: `reconcile_sprint()` does NOT update `s->cpu_cycles`. That's only done at the end of each sprint
iteration. This allows `current_cpu_cycles()` to compute the correct time during the sprint.

## Event Processing

Events are processed at the end of each sprint iteration:

```
┌─────────────────────────────────────────────────────────────────┐
│               EVENT PROCESSING                                   │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  process_event_queue(&cpu_events, cpu_cycles)                   │
│      │                                                           │
│      └── while (first_event->timestamp <= cpu_cycles):          │
│              │                                                   │
│              ├── Remove event from queue                        │
│              ├── Call event->callback(source, data)             │
│              │       └── Callback may schedule new events       │
│              └── Free event                                      │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

## Cycle Budget Calculation

Each sprint iteration calculates how many cycles to execute before the next event:

```c
if (cpu_events != NULL) {
    cycles_to_next_event = cpu_events->timestamp - cpu_cycles;
    cycles_to_execute = MIN(cycles_to_next_event, remaining_cycles);
}

instructions_to_execute = cycles_to_execute / AVG_CYCLES_PER_INSTR;

// Ensure at least 1 instruction if cycles > 0
if (cycles_to_execute > 0 && instructions_to_execute == 0)
    instructions_to_execute = 1;
```

## Instruction Atomicity and Event Timing

CPU instructions are executed atomically — we cannot stop mid-instruction. This creates a timing characteristic where
events may be "overshot" by a small number of cycles.

### The Overshoot Scenario

Consider an event scheduled at timestamp T, when we're at cpu_cycles = T - N:

1. **If N >= AVG_CYCLES_PER_INSTR**: We can execute `floor(N / CPI)` instructions and stop before the event.

2. **If 0 < N < AVG_CYCLES_PER_INSTR**: We must execute at least 1 instruction (CPI cycles), which overshoots by
   `CPI - N` cycles.

Example with CPI = 12 (hardware accuracy mode):
- Event at T+11 (11 cycles away)
- `cycles_to_instructions(11)` returns 1 (since 11 < 12 but 11 > 0)
- We execute 1 instruction = 12 cycles
- cpu_cycles advances from T to T+12
- Event at T+11 is now 1 cycle "in the past"

### Handling Overshoot

The scheduler handles this by:

1. **Relaxed Validation**: `validate_cpu_events()` allows events to be up to `AVG_CYCLES_PER_INSTR` cycles in the past.

2. **Processing Past Events**: `process_event_queue()` processes all events with `timestamp <= cpu_cycles`, including
   slightly overshot ones.

3. **Callback Awareness**: Event callbacks may schedule new events while other overshot events are still in the queue.

### Invariant Relaxation

Due to instruction atomicity, the invariant "all events have timestamp >= cpu_cycles" is relaxed to:
```
all events have timestamp + AVG_CYCLES_PER_INSTR >= cpu_cycles
```

This allows up to CPI-1 cycles of overshoot while still catching genuine timing bugs.

## Consistency Invariants

The following invariants must hold at all times:

### Sprint Counter Invariants

1. **Burndown <= Total**: `s->sprint_burndown <= s->sprint_total`
2. **After Sprint**: `s->sprint_total == 0` (between sprints)
3. **During Sprint**: `s->sprint_total > 0 && s->sprint_burndown >= 0`

### Cycle Counter Invariants

1. **Monotonic**: `cpu_cycles` only increases
2. **Event Ordering**: All events in queue have `timestamp >= cpu_cycles`
3. **Queue Order**: Events are sorted by increasing timestamp

### Event Queue Invariants

1. **Non-null Callbacks**: Every event has a non-NULL callback
2. **Finite Length**: Queue length is bounded (sanity check at 10000)
3. **Relaxed Timing**: First event timestamp + AVG_CYCLES_PER_INSTR >= cpu_cycles (allows for instruction overshoot)

## Assertion Strategy

All invariant checks use `GS_ASSERT()` or `GS_ASSERTF()` (defined in `common.h`), which provide file, line, and
function context on failure. There are no `printf()`-based error messages in the scheduler — all error reporting goes
through the assert macros.

Invariants are checked at key transition points via the `CHECK_INVARIANTS(s)` macro, which calls
`scheduler_check_invariants()` with the current function name as context.

## Checkpoint Save/Restore

### Saving

The scheduler saves:
- Plain-data fields up to `event_types` (via `system_write_checkpoint_data`)
- Event queue in checkpoint-friendly format (names instead of pointers)

### Restoring

```c
// Restore plain-data portion from checkpoint
system_read_checkpoint_data(checkpoint, s, ...);

// Reconstruct instruction count from cycles using RESTORED mode
uint32_t restored_cpi = (s->mode == schedule_hw_accuracy) ? 12 : 4;
s->total_instructions = s->cpu_cycles / restored_cpi;
s->sprint_total = 0;
s->sprint_burndown = 0;
```

**Critical**: We must use the restored mode's CPI, not the current global scheduler's CPI, because during restore the
global scheduler pointer still points to the old scheduler.

Event restoration is deferred: event data is saved in temporary storage (`tmp_events`) and resolved to live
pointers in `scheduler_start()` after all devices have registered their event types.

## Mode Switching

When scheduler mode changes at runtime:

1. `AVG_CYCLES_PER_INSTR` changes immediately
2. `cpu_cycles` remains unchanged (it's authoritative)
3. Future instruction-to-cycle conversions use new CPI
4. This creates a discontinuity in the instruction-to-cycle relationship

**Important**: This is why we cannot use `cpu_instr_count() * AVG_CYCLES_PER_INSTR == cpu_cycles` as an invariant —
it's only valid when CPI has been constant since boot.

## Debugging

### Useful Shell Commands

- `events` — Show pending event queue with timestamps
- `schedule [max|real|hw]` — Show or set scheduler mode
- `run [instructions]` — Start execution, optionally stop after N instructions
